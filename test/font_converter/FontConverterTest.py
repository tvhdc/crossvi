#!/usr/bin/env python3

import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT_DIR = REPO / "lib" / "EpdFont" / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
SPEC = importlib.util.spec_from_file_location("fontconvert_sdcard", SCRIPT_DIR / "fontconvert_sdcard.py")
CONVERTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONVERTER)
if importlib.util.find_spec("yaml") is None:
    # The catalog builder only needs PyYAML when it is executed.  Keep these
    # contract tests runnable in the dependency-light host/CI test environment.
    sys.modules["yaml"] = types.ModuleType("yaml")
BUILDER_SPEC = importlib.util.spec_from_file_location("build_sd_fonts", SCRIPT_DIR / "build-sd-fonts.py")
BUILDER = importlib.util.module_from_spec(BUILDER_SPEC)
BUILDER_SPEC.loader.exec_module(BUILDER)
MANIFEST_SPEC = importlib.util.spec_from_file_location(
    "generate_font_manifest", REPO / "scripts" / "generate-font-manifest.py")
MANIFEST = importlib.util.module_from_spec(MANIFEST_SPEC)
MANIFEST_SPEC.loader.exec_module(MANIFEST)


class FontConverterContractTest(unittest.TestCase):
    def test_manifest_reads_each_font_once_for_both_checksums(self):
        with tempfile.TemporaryDirectory() as tmp:
            font_path = Path(tmp) / "Example_14.cpfont"
            payload = b"cpfont-test-payload"
            font_path.write_bytes(payload)

            with mock.patch("builtins.open", wraps=open) as open_spy:
                crc32, sha256 = MANIFEST.compute_checksums(font_path)

            self.assertEqual(open_spy.call_count, 1)
            self.assertEqual(crc32, MANIFEST.zlib.crc32(payload) & 0xFFFFFFFF)
            self.assertEqual(sha256, hashlib.sha256(payload).hexdigest())

    def test_manifest_loads_description_and_metadata_from_one_yaml_parse(self):
        yaml_module = sys.modules.get("yaml")
        if not yaml_module or not hasattr(yaml_module, "safe_load"):
            self.skipTest("PyYAML is required for manifest metadata checks")
        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "fonts.yaml"
            config_path.write_text(
                "families:\n"
                "  - name: Example\n"
                "    description: Example font\n"
                "    license: OFL-1.1\n",
                encoding="utf-8",
            )
            with mock.patch.object(yaml_module, "safe_load", wraps=yaml_module.safe_load) as load_spy:
                descriptions, metadata = MANIFEST.load_family_data_from_yaml(config_path)

            self.assertEqual(load_spy.call_count, 1)
            self.assertEqual(descriptions, {"Example": "Example font"})
            self.assertEqual(metadata, {"Example": {"license": "OFL-1.1"}})

    def test_manifest_records_sha256_for_every_font_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            font_path = Path(tmp) / "Example_14.cpfont"
            payload = b"cpfont-test-payload"
            font_path.write_bytes(payload)
            original_reader = MANIFEST.read_cpfont_styles
            MANIFEST.read_cpfont_styles = lambda _: ["regular"]
            try:
                manifest = MANIFEST.build_manifest(
                    {"Example": [font_path]}, "https://example.invalid/fonts/")
            finally:
                MANIFEST.read_cpfont_styles = original_reader

            entry = manifest["families"][0]["files"][0]
            self.assertEqual(entry["size"], len(payload))
            self.assertEqual(entry["sha256"], hashlib.sha256(payload).hexdigest())

    def test_vietnamese_reading_preset_is_self_contained(self):
        resolved = CONVERTER.resolve_intervals("vietnamese-reading")
        covered = {cp for start, end in resolved for cp in range(start, end + 1)}
        self.assertTrue(CONVERTER.VIETNAMESE_REQUIRED_CODEPOINTS <= covered)
        for cp in (ord("A"), ord("z"), 0x0102, 0x0111, 0x0309, 0x031B,
                   0x0323, 0x1EF9, 0x201C, 0x2026, 0x20AB, 0x20AC, 0xFFFD):
            self.assertIn(cp, covered)

    def test_runtime_and_converter_vietnamese_contracts_match(self):
        header = (REPO / "lib" / "EpdFont" / "VietnameseFontContract.h").read_text(encoding="utf-8")
        ranges_body = re.search(
            r"REQUIRED_RANGES\[\]\s*=\s*\{(.*?)\};", header, re.DOTALL).group(1)
        singleton_body = re.search(
            r"REQUIRED_SINGLETONS\[\]\s*=\s*\{(.*?)\};", header, re.DOTALL).group(1)
        runtime_ranges = [tuple(int(value, 16) for value in pair)
                          for pair in re.findall(r"\{0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)\}", ranges_body)]
        runtime_singletons = {int(value, 16)
                              for value in re.findall(r"0x([0-9A-Fa-f]+)", singleton_body)}
        self.assertEqual(runtime_ranges, CONVERTER.VIETNAMESE_REQUIRED_RANGES)
        self.assertEqual(runtime_singletons, CONVERTER.VIETNAMESE_REQUIRED_SINGLETONS)

    def test_common_latin1_vietnamese_letters_are_mandatory(self):
        for cp in (0x00C1, 0x00E9, 0x00CD, 0x00F5, 0x00DA, 0x00FD):
            self.assertIn(cp, CONVERTER.VIETNAMESE_REQUIRED_CODEPOINTS)

    def test_legacy_vietnamese_preset_keeps_old_meaning(self):
        self.assertEqual(CONVERTER.INTERVAL_PRESETS["vietnamese"],
                         [(0x01A0, 0x01B0), (0x1EA0, 0x1EF9)])

    def test_ipa_preset_and_gentium_catalog_cover_full_ipa_ranges(self):
        resolved = CONVERTER.resolve_intervals("ipa-chars")
        covered = {cp for start, end in resolved for cp in range(start, end + 1)}
        for cp in (0x0250, 0x02AF, 0x02B0, 0x02FF):
            self.assertIn(cp, covered)

        catalog = (SCRIPT_DIR / "sd-fonts.yaml").read_text(encoding="utf-8")
        gentium = re.search(
            r"- name: GentiumBookPlus\n(?P<body>.*?)(?=\n\s*- name:|\Z)",
            catalog,
            re.DOTALL,
        )
        self.assertIsNotNone(gentium)
        self.assertRegex(gentium.group("body"), r"intervals:[^\n]*\bipa-chars\b")

    def test_codepoint_report_is_explicit(self):
        self.assertEqual(CONVERTER.format_codepoints([0x0102, 0x1EA0]),
                         "U+0102, U+1EA0")

    def test_converter_rejects_font_without_regular_style(self):
        source = (REPO / "lib" / "EpdFont" / "builtinFonts" / "source" /
                  "NotoSerif" / "NotoSerif-Bold.ttf")
        result = subprocess.run([
            sys.executable, str(SCRIPT_DIR / "fontconvert_sdcard.py"),
            "--intervals", "vietnamese-reading", "--size", "12",
            "--style", "bold", str(source), "--output", os.devnull,
        ], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("regular style is required", result.stderr)

    def test_catalog_uses_vietnamese_contract_and_matching_fallback_styles(self):
        expected = {
            "regular": "NotoSans-Regular.ttf",
            "bold": "NotoSans-Bold.ttf",
            "italic": "NotoSans-Italic.ttf",
            "bolditalic": "NotoSans-BoldItalic.ttf",
        }
        self.assertEqual({style: path.name for style, path in BUILDER.DEFAULT_FALLBACK_FONTS.items()}, expected)
        catalog = (SCRIPT_DIR / "sd-fonts.yaml").read_text(encoding="utf-8")
        family_names = [line.split(":", 1)[1].strip()
                        for line in catalog.splitlines()
                        if line.lstrip().startswith("- name:")]
        intervals = [line.split(":", 1)[1].strip().strip('"\'')
                     for line in catalog.splitlines()
                     if line.lstrip().startswith("intervals:")]
        self.assertTrue(family_names)
        self.assertEqual(len(intervals), len(family_names))
        for name, value in zip(family_names, intervals):
            self.assertIn("vietnamese-reading", value.split(","), name)

    def test_noto_vietnamese_packs_cover_all_reader_sizes_and_real_styles(self):
        if not hasattr(BUILDER.yaml, "safe_load"):
            self.skipTest("PyYAML is required for catalog structure checks")
        catalog = BUILDER.yaml.safe_load((SCRIPT_DIR / "sd-fonts.yaml").read_text(encoding="utf-8"))
        families = {family["name"]: family for family in catalog["families"]}
        expected_sizes = list(range(12, 29, 2))
        expected_styles = {"regular", "bold", "italic", "bolditalic"}
        for name in ("NotoSerifVietnamese", "NotoSansVietnamese"):
            family = families[name]
            self.assertEqual(family["sizes"], expected_sizes)
            self.assertEqual(family["intervals"], "vietnamese-reading")
            self.assertEqual(set(family["styles"]), expected_styles)
            self.assertEqual(family["license"], "SIL Open Font License 1.1")
            self.assertTrue(family["provenance"])
            for style in expected_styles:
                self.assertIn("path", family["styles"][style])
                if style != "regular":
                    self.assertNotEqual(family["styles"][style]["path"], family["styles"]["regular"]["path"])


@unittest.skipUnless(os.environ.get("CROSSVI_FONT_INTEGRATION") == "1",
                     "set CROSSVI_FONT_INTEGRATION=1 for FreeType converter integration")
class FontConverterIntegrationTest(unittest.TestCase):
    def test_unencoded_standard_ligature_is_rasterized_from_gsub_glyph(self):
        from fontTools.ttLib import TTFont

        source = (REPO / "lib" / "EpdFont" / "builtinFonts" / "source" /
                  "NotoSerif" / "NotoSerif-Regular.ttf")
        with tempfile.TemporaryDirectory() as tmp:
            font_path = Path(tmp) / "NotoSerif-unencoded-fi.ttf"
            font = TTFont(source)
            removed = False
            for cmap_table in font["cmap"].tables:
                if 0xFB01 in cmap_table.cmap:
                    del cmap_table.cmap[0xFB01]
                    removed = True
            self.assertTrue(removed)
            font.save(font_path)
            font.close()

            overrides = CONVERTER.extract_ligature_glyph_indices_fonttools(font_path)
            self.assertIn(0xFB01, overrides)
            raster = CONVERTER.rasterize_font_style(str(font_path), 12, [(0xFB01, 0xFB01)])

            self.assertEqual(raster.intervals, [(0xFB01, 0xFB01)])
            self.assertEqual(len(raster.all_glyphs), 1)
            glyph, bitmap = raster.all_glyphs[0]
            self.assertGreater(glyph.width, 0)
            self.assertGreater(len(bitmap), 0)

    def test_style_matched_fallback_fills_missing_codepoints(self):
        source_root = REPO / "lib" / "EpdFont" / "builtinFonts" / "source"
        ubuntu = source_root / "Ubuntu" / "Ubuntu-Regular.ttf"
        noto = source_root / "NotoSans" / "NotoSans-Regular.ttf"
        missing_without_fallback = CONVERTER.missing_codepoints_for_fonts(
            ubuntu, None, CONVERTER.VIETNAMESE_REQUIRED_CODEPOINTS)
        self.assertIn(0x0309, missing_without_fallback)
        self.assertEqual(CONVERTER.missing_codepoints_for_fonts(
            ubuntu, noto, CONVERTER.VIETNAMESE_REQUIRED_CODEPOINTS), [])

    def test_fallback_that_also_lacks_vietnamese_is_reported(self):
        source_root = REPO / "lib" / "EpdFont" / "builtinFonts" / "source"
        ubuntu_regular = source_root / "Ubuntu" / "Ubuntu-Regular.ttf"
        ubuntu_bold = source_root / "Ubuntu" / "Ubuntu-Bold.ttf"
        missing = CONVERTER.missing_codepoints_for_fonts(
            ubuntu_regular, ubuntu_bold, CONVERTER.VIETNAMESE_REQUIRED_CODEPOINTS)
        self.assertIn(0x0309, missing)
        self.assertIn("U+0309", CONVERTER.format_codepoints(missing))

    def test_four_style_output_is_deterministic_and_complete(self):
        noto = REPO / "lib" / "EpdFont" / "builtinFonts" / "source"
        sources = {
            "regular": noto / "NotoSerif" / "NotoSerif-Regular.ttf",
            "bold": noto / "NotoSerif" / "NotoSerif-Bold.ttf",
            "italic": noto / "NotoSerif" / "NotoSerif-Italic.ttf",
            "bolditalic": noto / "NotoSerif" / "NotoSerif-BoldItalic.ttf",
        }
        with tempfile.TemporaryDirectory() as tmp:
            hashes = []
            for index in range(2):
                output = Path(tmp) / f"CrossVi-{index}.cpfont"
                command = [
                    sys.executable, str(SCRIPT_DIR / "fontconvert_sdcard.py"),
                    "--intervals", "vietnamese-reading", "--require-vietnamese",
                    "--size", "12", "--output", str(output),
                ]
                for style, source in sources.items():
                    command.extend([f"--{style}", str(source)])
                subprocess.run(command, check=True, capture_output=True, text=True)
                hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(hashes[0], hashes[1])


if __name__ == "__main__":
    unittest.main()
