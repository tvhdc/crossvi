'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const flasher = require('../../docs/tools/flasher/flasher-core.js');

function setU32(bytes, offset, value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >>> 8) & 0xff;
  bytes[offset + 2] = (value >>> 16) & 0xff;
  bytes[offset + 3] = (value >>> 24) & 0xff;
}

function partitionBinary(partitions) {
  const types = {
    'app-ota_0': [0x00, 0x10],
    'app-ota_1': [0x00, 0x11],
    'data-ota': [0x01, 0x00],
    'data-nvs': [0x01, 0x02],
    'data-spiffs': [0x01, 0x82],
    'data-coredump': [0x01, 0x03]
  };
  const bytes = new Uint8Array(0x1000).fill(0xff);
  partitions.forEach((partition, index) => {
    const offset = index * 32;
    bytes[offset] = 0xaa;
    bytes[offset + 1] = 0x50;
    [bytes[offset + 2], bytes[offset + 3]] = types[partition.type];
    setU32(bytes, offset + 4, partition.offset);
    setU32(bytes, offset + 8, partition.size);
  });
  return bytes;
}

function firmwareImage(chipId = 0x0005) {
  const data = new Uint8Array(48);
  data[0] = 0xe9;
  data[1] = 1;
  data[12] = chipId & 0xff;
  data[13] = chipId >>> 8;
  setU32(data, 28, 3);
  data.set([1, 2, 3], 32);
  data[47] = 0xef ^ 1 ^ 2 ^ 3;
  return data;
}

async function testFirmwareSelectionRace() {
  const source = fs.readFileSync(path.join(__dirname, '../../docs/tools/flasher/flasher-ui.js'), 'utf8');
  const windowListeners = {};
  const elements = new Map();
  const flashSteps = Array.from({ length: 4 }, () => ({ dataset: {} }));

  function element(id) {
    const listeners = {};
    const value = {
      id,
      files: [],
      disabled: false,
      value: 0,
      textContent: '',
      className: '',
      dataset: {},
      classList: { add() {}, remove() {}, toggle() {} },
      addEventListener(type, callback) { listeners[type] = callback; },
      listeners
    };
    elements.set(id, value);
    return value;
  }

  [
    'firmwareFile', 'firmwareFileStatus', 'flashStart', 'flashStatus', 'flashBadge',
    'flashProgress', 'flashProgressText', 'flashDevice', 'flashTarget', 'flashBrowserWarning'
  ].forEach(element);

  let rejectPort;
  class MockWebFlasher {}
  MockWebFlasher.requestPort = () => new Promise((resolve, reject) => { rejectPort = reject; });
  const window = {
    CrossViFlasher: {
      CrossViWebFlasher: MockWebFlasher,
      validateFirmwareImage: async () => {}
    },
    CrossViTools: {
      t: (key, values = {}) => values.name ? `${key}:${values.name}` : key,
      formatSize: size => String(size)
    },
    addEventListener(type, callback) {
      (windowListeners[type] ||= []).push(callback);
    }
  };
  const context = {
    window,
    document: {
      getElementById: id => elements.get(id),
      querySelectorAll: selector => selector === '[data-flash-step]' ? flashSteps : []
    },
    navigator: { serial: {} },
    crypto: { subtle: {} },
    Uint8Array,
    console
  };
  vm.runInNewContext(source, context, { filename: 'flasher-ui.js' });
  windowListeners.DOMContentLoaded[0]();

  const fileInput = elements.get('firmwareFile');
  const fileStatus = elements.get('firmwareFileStatus');
  let finishOldRead;
  fileInput.files = [{
    name: 'old.bin',
    size: 1,
    arrayBuffer: () => new Promise(resolve => { finishOldRead = resolve; })
  }];
  const oldSelection = fileInput.listeners.change();

  fileInput.files = [{
    name: 'new.bin',
    size: 1,
    arrayBuffer: async () => Uint8Array.of(2).buffer
  }];
  await fileInput.listeners.change();
  finishOldRead(Uint8Array.of(1).buffer);
  await oldSelection;
  assert.equal(fileStatus.textContent, 'flashFileReady:new.bin');

  let finishLatestRead;
  fileInput.files = [{
    name: 'latest.bin',
    size: 1,
    arrayBuffer: () => new Promise(resolve => { finishLatestRead = resolve; })
  }];
  const latestSelection = fileInput.listeners.change();
  assert.equal(elements.get('flashStart').disabled, true,
    'choosing another file must disable Install until validation finishes');
  finishLatestRead(Uint8Array.of(3).buffer);
  await latestSelection;

  const flash = elements.get('flashStart').listeners.click();
  const disabledWhileBusy = fileInput.disabled;
  elements.get('flashStart').textContent = 'flashStart';
  windowListeners['crossvi-language-change'][0]();
  assert.equal(elements.get('flashStart').textContent, 'flashWorking',
    'changing language while flashing must preserve the busy button state');
  rejectPort(Object.assign(new Error('cancelled'), { name: 'NotFoundError' }));
  await flash;
  assert.equal(disabledWhileBusy, true, 'firmware selection must be disabled while flashing');
  assert.equal(fileInput.disabled, false, 'firmware selection must be restored after flashing');

  MockWebFlasher.requestPort = async () => ({});
  MockWebFlasher.prototype.flashFirmware = async (_firmware, options) => {
    options.onStep(0, 'running');
    options.onStep(0, 'done');
    options.onStep(1, 'running');
    options.onStep(1, 'done');
    assert.equal(flashSteps[1].dataset.state, 'running',
      'the combined device-check step must stay active until both checks finish');
    options.onStep(2, 'running');
    options.onStep(2, 'done');
    options.onStep(3, 'running');
    options.onStep(3, 'done');
    options.onStep(4, 'running');
    options.onStep(4, 'done');
    options.onStep(5, 'running');
    options.onStep(5, 'done');
  };
  await elements.get('flashStart').listeners.click();
  assert.deepEqual(flashSteps.map(step => step.dataset.state), ['done', 'done', 'done', 'done']);
}

(async () => {
  const legacy = flasher.parsePartitionTable(partitionBinary(flasher.LEGACY_PARTITIONS));
  const standard = flasher.parsePartitionTable(partitionBinary(flasher.CROSSVI_PARTITIONS));
  assert.equal(flasher.identifyLayout(legacy).name, 'CrossVi legacy');
  assert.equal(flasher.identifyLayout(standard).name, 'CrossVi standard');

  const unknown = standard.map(partition => ({ ...partition }));
  unknown[2].size -= 0x1000;
  assert.throws(() => flasher.identifyLayout(unknown), error => error.code === 'partition-unsupported');

  const otadata = new Uint8Array(0x2000).fill(0xff);
  const first = flasher.parseOtadata(otadata);
  assert.deepEqual(
    { active: first.activeApp, inactive: first.inactiveApp, sequence: first.newSequence, sector: first.targetSector },
    { active: 0, inactive: 1, sequence: 2, sector: 0 }
  );
  otadata.set(flasher.buildOtadataSector(otadata.subarray(0, 0x1000), first.newSequence), 0);
  flasher.verifyOtaSwitch(otadata, 1, 2);
  const second = flasher.parseOtadata(otadata);
  assert.deepEqual(
    { active: second.activeApp, inactive: second.inactiveApp, sequence: second.newSequence, sector: second.targetSector },
    { active: 1, inactive: 0, sequence: 3, sector: 1 }
  );

  await flasher.validateFirmwareImage(firmwareImage());
  await assert.rejects(flasher.validateFirmwareImage(firmwareImage(0x0009)), error => error.code === 'firmware-chip');
  await assert.rejects(flasher.validateFirmwareImage(firmwareImage(0xffff)), error => error.code === 'firmware-chip');
  const corrupt = firmwareImage();
  corrupt[32] ^= 1;
  await assert.rejects(flasher.validateFirmwareImage(corrupt), error => error.code === 'firmware-checksum');

  const html = fs.readFileSync(path.join(__dirname, '../../docs/tools/index.html'), 'utf8');
  assert.match(html, /id="flashTab"/);
  assert.match(html, /id="flashTool"/);
  assert.match(html, /flasher\/flasher-core\.js/);
  assert.match(html, /flasher\/flasher-ui\.js/);
  assert.match(html, /\["sleep", "vocabulary", "flash"\]/);
  assert.doesNotMatch(html, /Erase entire flash|Xóa toàn bộ flash/i);
  assert.equal((html.match(/data-flash-step=/g) || []).length, 4,
    'the firmware installer should present four user-facing steps');
  assert.doesNotMatch(html, /flashStep(?:Connect|Layout|Ota|Write|Verify|Restart)Hint/,
    'technical step explanations should not be shown in the main flow');
  assert.match(html, /<details class="flashTechnical">/,
    'technical device details should remain available on demand');
  assert.doesNotMatch(
    html,
    /for \(const el of \[els\.device, els\.mode, els\.fit, els\.zoom, els\.dither\]\) \{\s*el\.addEventListener\("input", render\);\s*el\.addEventListener\("change", render\);/,
    'sleep-image controls must not run the full conversion twice for one committed change'
  );
  await testFirmwareSelectionRace();

  const otaRaw = new Uint8Array(0x2000).fill(0xff);
  const writes = [];
  const webFlasher = new flasher.CrossViWebFlasher({});
  webFlasher.connect = async () => {
    webFlasher.loader = {
      readFlash: async (offset, size) => {
        if (offset === 0x8000) return partitionBinary(flasher.CROSSVI_PARTITIONS);
        if (offset === 0xe000 && size === otaRaw.length) return otaRaw;
        throw new Error(`Unexpected read at ${offset.toString(16)}`);
      },
      writeFlash: async options => {
        const [{ data, address }] = options.fileArray;
        assert.ok(data instanceof Uint8Array, 'esptool-js 0.6.x requires Uint8Array flash data');
        writes.push({ data, address });
        if (address === 0xe000) otaRaw.set(data, 0);
      }
    };
  };
  webFlasher.disconnect = async () => {};
  let detected = null;
  const result = await webFlasher.flashFirmware(firmwareImage(), {
    validated: true,
    onDevice: device => { detected = device; }
  });
  assert.deepEqual(detected, { layout: 'CrossVi standard', slot: 'app1' });
  assert.deepEqual(result, { layout: 'CrossVi standard', slot: 'app1' });
  assert.deepEqual(writes.map(write => write.address), [0x650000, 0xe000]);

  console.log('FlashToolTest: PASS');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
