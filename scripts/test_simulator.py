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
    font_family: int = 0, line_spacing: int = 1,
) -> None:
    payload = bytearray(48)
    payload[0] = 1 if custom else 0
    payload[1] = font_family
    payload[2] = font_size
    payload[3] = line_spacing
    payload[4] = 0
    payload[5] = 0
    payload[6] = 5
    payload[7] = 1
    payload[10] = 1
    payload[11] = 1
    encoded_name = sd_font_family.encode("utf-8")
    if len(encoded_name) >= 32:
        raise AssertionError("Simulator per-book font fixture name is too long")
    payload[14:14 + len(encoded_name)] = encoded_name
    payload[47] = 1  # EpubRenderMode::Balanced when no per-book render override is present.
    path.write_bytes(b"CVRS" + bytes([4]) + struct.pack("<H", len(payload)) + struct.pack("<I", zlib.crc32(payload)) + payload)


def read_per_book_reader_settings(path: Path) -> tuple[int, int, int, int, str]:
    data = path.read_bytes()
    if len(data) != 59 or data[:4] != b"CVRS" or data[4] != 4 or struct.unpack_from("<H", data, 5)[0] != 48:
        raise AssertionError(f"Invalid per-book reader settings fixture: {path}")
    payload = data[11:]
    if zlib.crc32(payload) != struct.unpack_from("<I", data, 7)[0]:
        raise AssertionError(f"Invalid per-book reader settings CRC: {path}")
    family_name = payload[14:46].split(b"\0", 1)[0].decode("utf-8")
    return payload[0], payload[1], payload[2], payload[3], family_name


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
    device: str, book_format: str, normalization: str, darkness: int, font_family: int = 0,
    font_size: int = 1, sd_font_family: Path | None = None,
) -> tuple[list[int], str]:
    family_key = sd_font_family.name if sd_font_family else str(font_family)
    output = BUILD / f"{device}-typography-{family_key}-{font_size}-{book_format}-{normalization.lower()}-{darkness}"
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
        "textDarkness": darkness,
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
    missed = [int(value) for value in re.findall(r"\((\d+) missed\)", log)]
    # Built-ins report explicit prewarm counts. SD fonts prewarm through their
    # bounded on-demand cache and do not emit the same summary line.
    if (sd_font_family is None and not missed) or any(missed):
        raise AssertionError(f"{device.upper()} {book_format.upper()} {normalization} missed glyphs: {missed}\n{log}")
    _, _, pixels = bmp_grayscale_pixels(find_single(shots, "*.bmp"))
    return pixels, log


def smoke_large_sd_font_typography(device: str, family_dir: Path) -> None:
    size20, log20 = run_typography_case(device, "txt", "NFC", 0, font_size=4, sd_font_family=family_dir)
    size28, log28 = run_typography_case(device, "txt", "NFD", 0, font_size=8, sd_font_family=family_dir)
    for point_size, log in ((20, log20), (28, log28)):
        if not re.search(rf"Loaded .* size={point_size}\b", log):
            raise AssertionError(f"{device.upper()} did not load the physical {point_size} pt SD font")
    if not any(pixel < 255 for pixel in size20) or not any(pixel < 255 for pixel in size28):
        raise AssertionError(f"{device.upper()} large SD font rendered no text")

    normal, normal_log = run_typography_case(
        device, "epub", "NFC", 0, font_size=8, sd_font_family=family_dir
    )
    decomposed, _ = run_typography_case(device, "epub", "NFD", 0, font_size=8, sd_font_family=family_dir)
    dark, _ = run_typography_case(device, "epub", "NFC", 1, font_size=8, sd_font_family=family_dir)
    extra_dark, _ = run_typography_case(device, "epub", "NFC", 2, font_size=8, sd_font_family=family_dir)
    if normal != decomposed:
        raise AssertionError(f"{device.upper()} 28 pt SD font differs for NFC/NFD text")
    normal_mask = [pixel < 255 for pixel in normal]
    if normal_mask != [pixel < 255 for pixel in dark] or normal_mask != [pixel < 255 for pixel in extra_dark]:
        raise AssertionError(f"{device.upper()} 28 pt text darkness changed typography geometry")
    if not re.search(r"Loaded .* size=28\b", normal_log):
        raise AssertionError(f"{device.upper()} EPUB did not use the 28 pt SD font")
    print(f"{device.upper()}: 20/28 pt SD font content with four styles and text-darkness smoke passed")


def smoke_vietnamese_typography(device: str) -> None:
    if unicodedata.normalize("NFC", TYPOGRAPHY_LINES[1]) == unicodedata.normalize("NFD", TYPOGRAPHY_LINES[1]):
        raise AssertionError("Vietnamese typography fixture does not exercise normalization")

    txt_nfc, txt_nfc_log = run_typography_case(device, "txt", "NFC", 0)
    txt_nfd, txt_nfd_log = run_typography_case(device, "txt", "NFD", 0)
    txt_nfc_pages = re.search(r"Built page index: (\d+) pages", txt_nfc_log)
    txt_nfd_pages = re.search(r"Built page index: (\d+) pages", txt_nfd_log)
    if not txt_nfc_pages or not txt_nfd_pages or txt_nfc_pages.group(1) != txt_nfd_pages.group(1):
        raise AssertionError(f"{device.upper()} TXT NFC/NFD pagination differs")
    if not any(pixel < 255 for pixel in txt_nfc) or not any(pixel < 255 for pixel in txt_nfd):
        raise AssertionError(f"{device.upper()} TXT typography fixture rendered no ink")

    normal, _ = run_typography_case(device, "epub", "NFC", 0)
    decomposed, _ = run_typography_case(device, "epub", "NFD", 0)
    if normal != decomposed:
        raise AssertionError(f"{device.upper()} EPUB NFC/NFD typography differs after normalization")
    dark, _ = run_typography_case(device, "epub", "NFC", 1)
    extra_dark, _ = run_typography_case(device, "epub", "NFC", 2)
    if any(not (extra <= darker <= base) for base, darker, extra in zip(normal, dark, extra_dark)):
        raise AssertionError(f"{device.upper()} text darkness brightened at least one pixel")
    if not any(darker < base for base, darker in zip(normal, dark)):
        raise AssertionError(f"{device.upper()} Dark text did not differ from Normal")
    if not any(extra < darker for darker, extra in zip(dark, extra_dark)):
        raise AssertionError(f"{device.upper()} Extra Dark text did not differ from Dark")
    normal_mask = [pixel < 255 for pixel in normal]
    if normal_mask != [pixel < 255 for pixel in dark] or normal_mask != [pixel < 255 for pixel in extra_dark]:
        raise AssertionError(f"{device.upper()} text darkness changed typography geometry")

    sans_txt, _ = run_typography_case(device, "txt", "NFC", 0, 1)
    sans_epub, _ = run_typography_case(device, "epub", "NFD", 0, 1)
    if not any(pixel < 255 for pixel in sans_txt) or not any(pixel < 255 for pixel in sans_epub):
        raise AssertionError(f"{device.upper()} Noto Sans typography fixture rendered no ink")
    print(f"{device.upper()}: Noto Serif/Sans Vietnamese typography and text-darkness smoke passed")


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
    find_single(sd / ".crosspoint", "xtc_*/cover.bmp")
    recent = json.loads((sd / ".crosspoint" / "recent.json").read_text(encoding="utf-8"))
    recent_paths = [entry.get("path") for entry in recent.get("books", [])]
    if f"/converter.{suffix}" not in recent_paths:
        raise AssertionError(f"{device.upper()} did not retain {suffix.upper()} in Recent Books")

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
    print(f"{device.upper()}: {suffix.upper()} File Browser/open/progress/cover/thumbnail/sleep smoke passed")


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
            index // width for index, (hidden, status) in enumerate(zip(hidden_pixels, status_pixels)) if hidden != status
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
    fixture = sd / "stress.xtc"
    source = XTC_FIXTURES / "crossvi-converter-480x800.xtc"
    create_navigation_fixture(source, fixture)

    first_turn_at = 3500
    # Keep every press visible across at least one ~16 ms simulator polling
    # cycle.  A 3 ms pulse can begin and end between two polls, which stresses
    # the input script rather than the reader state/render contract.
    turn_interval = 32
    press_duration = 16
    events = ["1200:CONFIRM", "2600:CONFIRM"]
    for index in range(1000):
        key = "RIGHT" if index % 2 == 0 else "LEFT"
        events.append(f"{first_turn_at + index * turn_interval}:{key}:{press_duration}")
    screenshot_at = first_turn_at + 1000 * turn_interval + 1200
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
    print("X3: 1,000 rapid XTC page-turn requests kept bitmap and progress on page 1")


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
    settings = {"uiTheme": 5}
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
    # actual menu items and not stop on the read-only Today/Goal card. X4 uses
    # Recent Books so both action-popup callers get exercised across the pair.
    enter = (
        "1000:DOWN,1400:DOWN,1800:DOWN,2200:DOWN,2600:DOWN,3000:DOWN,3600:CONFIRM"
        if source == "browser"
        else "1200:DOWN,1700:DOWN,2300:CONFIRM"
    )
    events = [
        "800:SCREENSHOT",
        enter,
        "5000:CONFIRM:1000",  # popup must appear at 5500, before release at 6000
        "5600:SCREENSHOT",
        "6200:SCREENSHOT",
        "7600:BACK",
        "8100:UP",
        "8600:CONFIRM",
        "9400:CONFIRM",  # type the initially selected '1'
        "9800:SCREENSHOT",
        "10200:DOWN,10600:DOWN,11000:DOWN,11400:DOWN",
        "11800:RIGHT,12200:RIGHT,12600:RIGHT,13000:RIGHT",
        "13400:CONFIRM",
        "14400:SCREENSHOT",
        "15000:BACK,15500:UP,16000:CONFIRM",
        "17000:SCREENSHOT",
        "17500:DOWN,17900:DOWN,18300:DOWN,18700:DOWN",
        "19100:RIGHT,19500:RIGHT,19900:RIGHT,20300:RIGHT",
        "20700:CONFIRM",
        "21500:CONFIRM",
    ]
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
    activity = "FileBrowser" if source == "browser" else "RecentBooks"
    if f"Entering activity: {activity}" not in log or log.count("Entering activity: KeyboardEntry") < 2:
        raise AssertionError(f"{device.upper()} did not complete the {source} search flow:\n{log}")
    if "Entering activity: TxtReader" not in log:
        raise AssertionError(f"{device.upper()} did not open the wrapped first search result:\n{log}")

    screenshots = sorted(shots.glob("*.bmp"))
    if len(screenshots) != 6:
        raise AssertionError(f"{device.upper()} {source} search produced {len(screenshots)} screenshots")
    if screenshots[1].read_bytes() != screenshots[2].read_bytes():
        raise AssertionError(f"{device.upper()} Confirm release selected the first popup action")
    if screenshots[3].read_bytes() != screenshots[5].read_bytes():
        raise AssertionError(f"{device.upper()} did not preserve the query when reopening search")

    recent = json.loads((control / "recent.json").read_text(encoding="utf-8"))
    if recent.get("pinned", []) != initial_pinned:
        raise AssertionError(f"{device.upper()} popup release changed the pinned-book list")
    if not recent.get("books") or recent["books"][0].get("path") != "/12-book-0.txt":
        raise AssertionError(f"{device.upper()} search did not open its first ranked result")
    print(f"{device.upper()}: {source} long-press/search/query-retention smoke passed")


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
            "CROSSVI_SIM_INPUT_SCRIPT": "1200:DOWN,1600:DOWN,2200:CONFIRM,3200:SCREENSHOT",
            "CROSSVI_SIM_EXIT_AFTER_MS": "4000",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=10)
    log = completed.stdout + completed.stderr
    if "Entering activity: ReadingStats" not in log:
        raise AssertionError(f"{device.upper()} could not open device stats from the Home menu:\n{log}")
    find_single(shots, "*.bmp")
    print(f"{device.upper()}: read-only Home summary kept the Reading Stats menu available")


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

    # Home -> Settings -> Reader -> Font size. Preview Large then cancel,
    # reopen, confirm Large, leave Settings and verify the persisted value.
    events = (
        "800:DOWN,1100:DOWN,1400:DOWN,1700:DOWN,2000:DOWN,2400:CONFIRM,"
        "3100:CONFIRM,3500:DOWN,3800:DOWN,4100:DOWN,4500:CONFIRM,"
        "5000:SCREENSHOT,5400:DOWN,5800:SCREENSHOT,6200:BACK,"
        "6900:CONFIRM,7300:DOWN,7700:CONFIRM,8400:BACK,8800:BACK"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "9500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    if log.count("Entering activity: FontSizeSelect") != 2:
        raise AssertionError(f"{device.upper()} did not complete both font-size picker flows:\n{log}")
    if "Outside range" in log or "page buffer slots full" in log:
        raise AssertionError(f"{device.upper()} font-size preview leaked drawing/cache state:\n{log}")
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("fontSize") != 2:
        raise AssertionError(f"{device.upper()} did not persist the confirmed Large font size")
    if len(list(shots.glob("*.bmp"))) != 2:
        raise AssertionError(f"{device.upper()} font-size picker did not render both preview states")
    print(f"{device.upper()}: font-size preview/cancel/confirm/persist smoke passed")


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
        json.dumps({"uiTheme": 5, "language": "VI" if device == "x3" else "EN", "fontSize": 1,
                    "sdFontFamilyName": family_name}) + "\n",
        encoding="utf-8",
    )

    # Enter the font-size picker, navigate from 14 to the exact 28 pt file,
    # cancel, then repeat and confirm. This covers scrolling and Back restore.
    events = (
        "800:DOWN,1100:DOWN,1400:DOWN,1700:DOWN,2000:DOWN,2400:CONFIRM,"
        "3100:CONFIRM,3500:DOWN,3800:DOWN,4100:DOWN,4500:CONFIRM,"
        "4900:DOWN,5200:DOWN,5500:DOWN,5800:DOWN,6100:DOWN,6400:DOWN,6700:DOWN,"
        "7100:SCREENSHOT,7500:BACK,8100:CONFIRM,"
        "8500:DOWN,8800:DOWN,9100:DOWN,9400:DOWN,9700:DOWN,10000:DOWN,10300:DOWN,"
        "10700:SCREENSHOT,11100:CONFIRM,11700:BACK,12100:BACK"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "12800",
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
        "800:DOWN,1100:DOWN,1400:DOWN,1700:DOWN,2000:DOWN,2400:CONFIRM,"
        "3100:CONFIRM,3500:DOWN,3800:DOWN,4100:DOWN,4500:CONFIRM,"
        "5200:DOWN,6100:BACK,6900:BACK,7500:BACK"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "8400",
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
            "7200:CONFIRM,7800:DOWN,8200:DOWN,8700:CONFIRM,9300:DOWN,"
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
            "6700:DOWN", "7200:CONFIRM", "7800:DOWN", "8300:CONFIRM",
        ]
        move_time = 8800
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
        if "Entering activity: FontSelect" not in log:
            raise AssertionError(f"{device.upper()} did not enter font selection for {case_name}:\n{log}")
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
            "CROSSVI_SIM_EXIT_AFTER_MS": "1600",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=10)
    log = completed.stdout + completed.stderr
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

    run([sys.executable, "scripts/setup_simulator_deps.py"])
    test_host_adapters()
    if not args.skip_build:
        pio = platformio()
        run([pio, "run", "-e", "simulator_x3"])
        run([pio, "run", "-e", "simulator_x4"])

    golden = json.loads(GOLDEN_PATH.read_text(encoding="utf-8"))
    for device in ("x3", "x4"):
        smoke_home_stats_menu(device)
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
        smoke_book_search_actions(device, "browser" if device == "x3" else "recent")
        raw_digest, bmp_digest = smoke_device(device, golden[device], args.update_golden)
        golden[device]["sha256"] = raw_digest
        golden[device]["bmp_sha256"] = bmp_digest
        smoke_xtc_fixture(device, "crossvi-converter-480x800.xtc")
        smoke_xtc_status_modes(device)
        smoke_xtc_fixture(device, "crossvi-converter-480x800.xtch")
        smoke_xtc_navigation(device)
        if device == "x3":
            stress_xtc_page_turns()
            smoke_xtc_replacement()
    if args.update_golden:
        GOLDEN_PATH.write_text(json.dumps(golden, indent=2) + "\n", encoding="utf-8")
        print(f"Updated {GOLDEN_PATH.relative_to(ROOT)}")
    print("Simulator checks passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
