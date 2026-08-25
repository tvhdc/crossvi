# Production UI languages

CrossVi production firmware builds exactly 16 user-interface languages by default. The single source of truth is
[`lib/I18n/build-languages.txt`](../lib/I18n/build-languages.txt). `scripts/gen_i18n.py` reads that ordered whitelist and
generates the `Language` enum, language selector, translated string blobs, offset tables, and UI character sets only for
those entries.

| Code | UI language | Translation source |
| --- | --- | --- |
| `EN` | English | `english.yaml` |
| `VI` | Vietnamese | `vietnamese.yaml` |
| `ES` | Spanish | `spanish.yaml` |
| `FR` | French | `french.yaml` |
| `DE` | German | `german.yaml` |
| `PT` | Portuguese (Brazil) | `portuguese-BR.yaml` |
| `IT` | Italian | `italian.yaml` |
| `PL` | Polish | `polish.yaml` |
| `RU` | Russian | `russian.yaml` |
| `UK` | Ukrainian | `ukrainian.yaml` |
| `TR` | Turkish | `turkish.yaml` |
| `ID` | Indonesian | `indonesia.yaml` |
| `NL` | Dutch | `dutch.yaml` |
| `CS` | Czech | `czech.yaml` |
| `SV` | Swedish | `swedish.yaml` |
| `P2` | Portuguese (Portugal) | `portuguese-PT.yaml` |

All other files in `lib/I18n/translations/` remain translation sources, but their strings, offsets, UI character sets,
and selector entries are not compiled into the default firmware.

## Enabling or disabling a language

1. Keep or add its YAML source in `lib/I18n/translations/`, including a unique `_language_code`.
2. Add or remove that code in `lib/I18n/build-languages.txt`. Keep `EN` first.
3. Run `python3 scripts/gen_i18n.py --strip-unused`.
4. Verify the generated selector and migration tests, then build `gh_release` and check Flash headroom.

Do not maintain another production-language list. The YAML directory is the translation catalog; the whitelist file is
the build configuration.

## Compatibility

Settings JSON stores a stable language code rather than an enum index. If a saved code is no longer built, CrossVi falls
back to English and rewrites the setting on the next successful settings save. The historical numeric `language.bin`
migration table maps unavailable languages to English, avoiding invalid or misinterpreted enum values.

This whitelist controls only firmware UI translations. EPUB/TXT/XTC content rendering uses the reader font and Unicode
pipelines independently. Built-in and SD-card font glyph ranges are not generated from this whitelist and must not be
removed when changing UI language support.
