'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
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

(async () => {
  const x3 = flasher.parsePartitionTable(partitionBinary(flasher.X3_PARTITIONS));
  const x4 = flasher.parsePartitionTable(partitionBinary(flasher.X4_PARTITIONS));
  assert.equal(flasher.identifyLayout(x3).model, 'X3');
  assert.equal(flasher.identifyLayout(x4).model, 'X4');

  const unknown = x4.map(partition => ({ ...partition }));
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

  const otaRaw = new Uint8Array(0x2000).fill(0xff);
  const writes = [];
  const webFlasher = new flasher.CrossViWebFlasher({});
  webFlasher.connect = async () => {
    webFlasher.loader = {
      readFlash: async (offset, size) => {
        if (offset === 0x8000) return partitionBinary(flasher.X4_PARTITIONS);
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
  await webFlasher.flashFirmware(firmwareImage(), { validated: true });
  assert.deepEqual(writes.map(write => write.address), [0x650000, 0xe000]);

  console.log('FlashToolTest: PASS');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
