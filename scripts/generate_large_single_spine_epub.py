#!/usr/bin/env python3
"""Generate the deterministic, copyright-free large-spine benchmark EPUB."""

from __future__ import annotations

import hashlib
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "test" / "epubs" / "benchmark_large_single_spine.epub"
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


def add(book: zipfile.ZipFile, name: str, content: str) -> None:
    info = zipfile.ZipInfo(name, ZIP_TIME)
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    book.writestr(info, content.encode("utf-8"))


def main() -> None:
    paragraphs = "".join(
        f'<p id="p{index:04d}">{index:04d} — Trên hành trình đọc sách, người đọc giữ nguyên vị trí, '
        "thử chuyển trang, mở menu và quay lại chương đang đọc. Nội dung tiếng Việt này tạo một spine dài, "
        "ổn định và không sử dụng văn bản có bản quyền.</p>"
        for index in range(2500)
    )
    chapter = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Large spine</title></head><body>'
        '<h1>CrossVi large single-spine benchmark</h1>'
        f"{paragraphs}</body></html>"
    )
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
        '<dc:title>CrossVi Large Single-Spine Benchmark</dc:title><dc:creator>CrossVi Tests</dc:creator>'
        '<dc:language>vi</dc:language><dc:identifier id="bookid">crossvi-benchmark-large-spine-v1</dc:identifier>'
        '</metadata><manifest><item id="chapter" href="chapter.xhtml" '
        'media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>'
    )

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(OUTPUT, "w") as book:
        add(book, "mimetype", "application/epub+zip")
        add(book, "META-INF/container.xml", container)
        add(book, "OEBPS/content.opf", package)
        add(book, "OEBPS/chapter.xhtml", chapter)
    print(f"{hashlib.sha256(OUTPUT.read_bytes()).hexdigest()}  {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
