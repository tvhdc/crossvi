'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const converter = require('../../docs/tools/vocabulary-converter/converter-core.js');

const rows = [
  ['Word', 'Meaning', 'Pronunciation', 'Part of speech'],
  ['hello', 'xin chào', 'həˈləʊ', 'other'],
  ['book', 'quyển sách', 'bʊk', 'noun'],
  ['read', 'đọc', 'riːd', 'verb'],
  ['quickly', 'nhanh chóng', 'ˈkwɪkli', 'adverb'],
  ['hello', 'xin chào', '', 'other'],
  ['', 'missing word', '', 'other']
];
const normalized = converter.normalizeRows(rows, { word:0, meaning:1, pronunciation:2, partOfSpeech:3, hasHeader:true });
assert.equal(normalized.entries.length, 4);
assert.equal(normalized.errors.length, 2);
assert.equal(normalized.entries[1].partOfSpeech, 0);

const encoded = converter.buildCvocab(normalized.entries, 'English - Vietnamese');
const info = converter.inspectCvocab(encoded);
assert.deepEqual(info, { count:4, title:'English - Vietnamese' });
assert.throws(() => converter.buildCvocab(normalized.entries.map(entry => ({...entry, meaning:'same', meaningBytes:new TextEncoder().encode('same')})), 'Bad'), /meaning-count/);

const toolsHtml = fs.readFileSync(path.join(__dirname, '../../docs/tools/index.html'), 'utf8');
assert.doesNotMatch(toolsHtml, /<iframe\b/i);
assert.doesNotMatch(toolsHtml, /Open tool directly/i);
assert.match(toolsHtml, /id="sleepTool"/);
assert.match(toolsHtml, /id="vocabularyTool"/);
assert.match(toolsHtml, /value="en">English<\/option>[\s\S]*value="vi">Tiếng Việt<\/option>/);
assert.doesNotMatch(toolsHtml, /value="zh"/);
assert.match(toolsHtml, /vocabGuideTitle: "Simple Excel format"/);
assert.match(toolsHtml, /vocabGuideTitle: "Định dạng Excel đơn giản"/);

const sleepRedirect = fs.readFileSync(path.join(__dirname, '../../docs/tools/sleep-image-converter/index.html'), 'utf8');
const vocabularyRedirect = fs.readFileSync(path.join(__dirname, '../../docs/tools/vocabulary-converter/index.html'), 'utf8');
assert.match(sleepRedirect, /\.\.\/\?tool=sleep/);
assert.match(vocabularyRedirect, /\.\.\/\?tool=vocabulary/);

console.log('VocabularyConverterTest: PASS');
