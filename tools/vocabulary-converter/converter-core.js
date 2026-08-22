(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.CrossViVocabulary = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const LIMITS = Object.freeze({ entries: 10000, word: 96, pronunciation: 96, meaning: 192, title: 60 });
  const HEADER_SIZE = 96;
  const RECORD_SIZE = 16;
  const encoder = new TextEncoder();

  function clean(value) {
    return String(value == null ? '' : value).replace(/[\t\r\n]+/g, ' ').trim();
  }

  function normalizePartOfSpeech(value) {
    const key = clean(value).toLowerCase().replace(/[\s_-]+/g, ' ');
    if (['noun', 'n', 'danh từ', '名词'].includes(key)) return 0;
    if (['verb', 'v', 'động từ', '动词'].includes(key)) return 1;
    if (['adjective', 'adj', 'tính từ', '形容词'].includes(key)) return 2;
    if (['adverb', 'adv', 'trạng từ', '副词'].includes(key)) return 3;
    return 4;
  }

  function fnv1a64(bytes) {
    let hash = 0xcbf29ce484222325n;
    for (const byte of bytes) {
      hash ^= BigInt(byte);
      hash = BigInt.asUintN(64, hash * 0x100000001b3n);
    }
    return hash;
  }

  function crc32(bytes) {
    if (!crc32.table) {
      crc32.table = new Uint32Array(256);
      for (let n = 0; n < 256; n++) {
        let value = n;
        for (let bit = 0; bit < 8; bit++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
        crc32.table[n] = value >>> 0;
      }
    }
    let value = 0xffffffff;
    for (const byte of bytes) value = crc32.table[(value ^ byte) & 0xff] ^ (value >>> 8);
    return (value ^ 0xffffffff) >>> 0;
  }

  function normalizeRows(rows, columns) {
    const entries = [];
    const errors = [];
    const duplicates = new Set();
    const start = columns.hasHeader ? 1 : 0;
    for (let rowIndex = start; rowIndex < rows.length && entries.length < LIMITS.entries; rowIndex++) {
      const row = rows[rowIndex] || [];
      const word = clean(row[columns.word]);
      const meaning = clean(row[columns.meaning]);
      const pronunciation = columns.pronunciation < 0 ? '' : clean(row[columns.pronunciation]);
      const partOfSpeech = columns.partOfSpeech < 0 ? 4 : normalizePartOfSpeech(row[columns.partOfSpeech]);
      if (!word && !meaning) continue;
      if (!word || !meaning) {
        errors.push({ row: rowIndex + 1, reason: 'missing' });
        continue;
      }
      const wordBytes = encoder.encode(word);
      const pronunciationBytes = encoder.encode(pronunciation);
      const meaningBytes = encoder.encode(meaning);
      if (wordBytes.length > LIMITS.word || pronunciationBytes.length > LIMITS.pronunciation ||
          meaningBytes.length > LIMITS.meaning) {
        errors.push({ row: rowIndex + 1, reason: 'long' });
        continue;
      }
      const duplicateKey = word + '\u0000' + meaning;
      if (duplicates.has(duplicateKey)) {
        errors.push({ row: rowIndex + 1, reason: 'duplicate' });
        continue;
      }
      duplicates.add(duplicateKey);
      entries.push({ word, pronunciation, meaning, partOfSpeech, wordBytes, pronunciationBytes, meaningBytes });
    }
    if (rows.length - start > LIMITS.entries) errors.push({ row: LIMITS.entries + start + 1, reason: 'limit' });
    return { entries, errors };
  }

  function buildCvocab(entries, requestedTitle) {
    if (!Array.isArray(entries) || entries.length < 4 || entries.length > LIMITS.entries)
      throw new Error('entry-count');
    const title = clean(requestedTitle) || 'Custom vocabulary';
    const titleBytes = encoder.encode(title);
    if (titleBytes.length > LIMITS.title) throw new Error('title-long');
    if (new Set(entries.map(entry => entry.meaning)).size < 4) throw new Error('meaning-count');

    let dataSize = 0;
    for (const entry of entries)
      dataSize += entry.wordBytes.length + entry.pronunciationBytes.length + entry.meaningBytes.length + 3;
    const indexSize = entries.length * RECORD_SIZE;
    const output = new Uint8Array(HEADER_SIZE + indexSize + dataSize);
    const view = new DataView(output.buffer);
    output.set([67, 86, 79, 67, 65, 66, 0, 1], 0); // CVOCAB\0\1
    view.setUint16(8, 1, true);
    view.setUint16(10, HEADER_SIZE, true);
    view.setUint32(12, entries.length, true);
    view.setUint32(16, HEADER_SIZE, true);
    view.setUint32(20, HEADER_SIZE + indexSize, true);
    view.setUint32(24, dataSize, true);
    view.setUint16(32, titleBytes.length, true);
    output.set(titleBytes, 36);

    let dataOffset = 0;
    for (let index = 0; index < entries.length; index++) {
      const entry = entries[index];
      const recordOffset = HEADER_SIZE + index * RECORD_SIZE;
      const recordLength = entry.wordBytes.length + entry.pronunciationBytes.length + entry.meaningBytes.length + 3;
      view.setUint32(recordOffset, dataOffset, true);
      view.setUint16(recordOffset + 4, recordLength, true);
      view.setUint8(recordOffset + 6, entry.partOfSpeech);
      view.setBigUint64(recordOffset + 8, fnv1a64(entry.meaningBytes), true);
      let cursor = HEADER_SIZE + indexSize + dataOffset;
      output.set(entry.wordBytes, cursor); cursor += entry.wordBytes.length + 1;
      output.set(entry.pronunciationBytes, cursor); cursor += entry.pronunciationBytes.length + 1;
      output.set(entry.meaningBytes, cursor);
      dataOffset += recordLength;
    }
    view.setUint32(28, crc32(output.subarray(HEADER_SIZE)), true);
    return output;
  }

  function inspectCvocab(bytes) {
    const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (data.length < HEADER_SIZE) throw new Error('short');
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const magic = [67, 86, 79, 67, 65, 66, 0, 1];
    if (!magic.every((value, index) => data[index] === value)) throw new Error('magic');
    const count = view.getUint32(12, true);
    const dataOffset = view.getUint32(20, true);
    const dataSize = view.getUint32(24, true);
    if (dataOffset + dataSize !== data.length || view.getUint32(28, true) !== crc32(data.subarray(HEADER_SIZE)))
      throw new Error('integrity');
    const titleLength = view.getUint16(32, true);
    return { count, title: new TextDecoder().decode(data.subarray(36, 36 + titleLength)) };
  }

  return { LIMITS, normalizeRows, buildCvocab, inspectCvocab };
});
