# Custom vocabulary sets

CrossVi can study either its built-in English-Vietnamese list or a custom list stored on the SD card. The firmware does not parse Excel workbooks directly. Instead, the browser converter validates the spreadsheet and creates a compact `.cvocab` file designed for the device.

## Create and install a set

1. Open the [CrossVi Vocabulary Set Maker](https://tvhdc.github.io/crossvi/tools/vocabulary-converter/).
2. Choose an `.xlsx`, `.xls`, `.csv`, or `.tsv` file. Only the first worksheet is read.
3. Select the columns containing the word or phrase and its meaning. Pronunciation and part of speech are optional.
4. Check the preview, then download the `.cvocab` file.
5. Upload the result through CrossVi **File Transfer** over Wi-Fi, or copy it anywhere on the SD card.
6. On the device, open **Vocabulary learning -> Vocabulary set -> Choose a file** and select it.

The converter runs in the browser. Spreadsheet contents are not uploaded to the CrossVi project. Loading the Excel library requires an internet connection; conversion after it has loaded is local.

## Recommended columns

| Word or phrase | Pronunciation | Meaning | Part of speech |
| --- | --- | --- | --- |
| example | /ig-zam-puhl/ | a representative case | noun |

Only the word and meaning columns are required. A set must contain at least four distinct meanings so a multiple-choice question can be generated.

## Limits

- Up to 10,000 entries and 8 MiB per `.cvocab` file.
- Word or phrase: at most 96 UTF-8 bytes.
- Pronunciation: at most 96 UTF-8 bytes.
- Meaning: at most 192 UTF-8 bytes.
- Empty rows are ignored. Rows without a word or meaning are rejected.
- The converter recognizes common English, Vietnamese, and Simplified Chinese part-of-speech names. Unknown values are stored as `Other`.

Review progress is stored separately for each custom set. Replacing a `.cvocab` file with different content starts a separate review state; it does not overwrite the built-in set's review progress.

## File format

`.cvocab` version 1 is a bounded, little-endian binary format with a 96-byte header, a fixed-size entry index, UTF-8 record data, and CRC32 content validation. The firmware validates file size, offsets, record lengths, answer diversity, and the CRC before making a set active. Invalid or truncated files are rejected without replacing the currently selected set.

Excel parsing intentionally stays in the web tool. Bundling a workbook parser into the ESP32-C3 firmware would consume substantially more Flash and RAM while offering no benefit after conversion.
