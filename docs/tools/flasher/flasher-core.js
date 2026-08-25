/*
 * CrossVi WebSerial OTA flasher.
 *
 * The partition parsing and OTA selection flow are adapted from CrossPoint
 * Tools (https://github.com/crosspoint-reader/crosspoint-tools), MIT licensed.
 * Only the Xteink X3/X4 OTA update path is retained here.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.CrossViFlasher = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const ESPTOOL_URL = "https://unpkg.com/esptool-js@0.6.0/bundle.js";
  const ESP_IMAGE_MAGIC = 0xe9;
  const ESP32_C3_CHIP_ID = 0x0005;
  const ESP_IMAGE_HEADER_SIZE = 24;
  const ESP_SEGMENT_HEADER_SIZE = 8;
  const ESP_SHA_TRAILER_SIZE = 32;
  const ESP_CHECKSUM_SEED = 0xef;
  const MAX_FIRMWARE_SIZE = 0x770000;
  const OTA_SECTOR_SIZE = 0x1000;
  const OTADATA_SIZE = OTA_SECTOR_SIZE * 2;
  const OTA_STATE_NEW = 0;
  const INVALID_OTA_STATES = new Set([3, 4]);

  const CROSSVI_PARTITIONS = [
    { type: "data-nvs", offset: 0x9000, size: 0x5000 },
    { type: "data-ota", offset: 0xe000, size: 0x2000 },
    { type: "app-ota_0", offset: 0x10000, size: 0x640000 },
    { type: "app-ota_1", offset: 0x650000, size: 0x640000 },
    { type: "data-spiffs", offset: 0xc90000, size: 0x360000 },
    { type: "data-coredump", offset: 0xff0000, size: 0x10000 }
  ];

  const LEGACY_PARTITIONS = [
    { type: "data-nvs", offset: 0x9000, size: 0x5000 },
    { type: "data-ota", offset: 0xe000, size: 0x2000 },
    { type: "app-ota_0", offset: 0x10000, size: 0x770000 },
    { type: "app-ota_1", offset: 0x780000, size: 0x770000 },
    { type: "data-spiffs", offset: 0xef0000, size: 0x100000 },
    { type: "data-coredump", offset: 0xff0000, size: 0x10000 }
  ];

  const PARTITION_TYPES = {
    0x00: { 0x10: "app-ota_0", 0x11: "app-ota_1" },
    0x01: { 0x00: "data-ota", 0x02: "data-nvs", 0x03: "data-coredump", 0x82: "data-spiffs" }
  };

  class FlashError extends Error {
    constructor(code, details = {}) {
      super(code);
      this.name = "FlashError";
      this.code = code;
      this.details = details;
    }
  }

  function fail(code, details) {
    throw new FlashError(code, details);
  }

  function leU32(bytes, offset = 0) {
    return (
      bytes[offset] +
      bytes[offset + 1] * 0x100 +
      bytes[offset + 2] * 0x10000 +
      bytes[offset + 3] * 0x1000000
    ) >>> 0;
  }

  function setLeU32(bytes, offset, value) {
    bytes[offset] = value & 0xff;
    bytes[offset + 1] = (value >>> 8) & 0xff;
    bytes[offset + 2] = (value >>> 16) & 0xff;
    bytes[offset + 3] = (value >>> 24) & 0xff;
  }

  function equalBytes(left, right) {
    if (left.length !== right.length) return false;
    for (let i = 0; i < left.length; i++) {
      if (left[i] !== right[i]) return false;
    }
    return true;
  }

  const CRC32_TABLE = new Uint32Array(256);
  for (let i = 0; i < CRC32_TABLE.length; i++) {
    let value = i;
    for (let bit = 0; bit < 8; bit++) {
      value = (value & 1) ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
    }
    CRC32_TABLE[i] = value >>> 0;
  }

  function crc32(bytes, initial = 0) {
    let value = initial === 0 ? 0 : (initial ^ 0xffffffff) >>> 0;
    for (const byte of bytes) value = CRC32_TABLE[(value ^ byte) & 0xff] ^ (value >>> 8);
    return (value ^ 0xffffffff) >>> 0;
  }

  function otaSequenceCrc(sequence) {
    const bytes = new Uint8Array(4);
    setLeU32(bytes, 0, sequence);
    return crc32(bytes, 0xffffffff);
  }

  async function validateFirmwareImage(data) {
    if (!(data instanceof Uint8Array)) fail("firmware-read");
    if (data.length < ESP_IMAGE_HEADER_SIZE) fail("firmware-small");
    if (data.length > MAX_FIRMWARE_SIZE) fail("firmware-large", { size: data.length });
    if (data[0] !== ESP_IMAGE_MAGIC) fail("firmware-magic");

    const chipId = data[12] | (data[13] << 8);
    if (chipId !== ESP32_C3_CHIP_ID) {
      fail("firmware-chip", { chipId });
    }

    const segmentCount = data[1];
    if (segmentCount < 1 || segmentCount > 16) fail("firmware-segments", { segmentCount });
    const hashAppended = (data[23] & 1) !== 0;
    let checksum = ESP_CHECKSUM_SEED;
    let position = ESP_IMAGE_HEADER_SIZE;

    for (let segment = 0; segment < segmentCount; segment++) {
      if (data.length - position < ESP_SEGMENT_HEADER_SIZE) fail("firmware-truncated");
      const dataLength = leU32(data, position + 4);
      position += ESP_SEGMENT_HEADER_SIZE;
      if (dataLength > data.length - position) fail("firmware-truncated");
      const end = position + dataLength;
      for (let i = position; i < end; i++) checksum ^= data[i];
      position = end;
    }

    const paddedEnd = (position + 16) & ~15;
    const expectedSize = paddedEnd + (hashAppended ? ESP_SHA_TRAILER_SIZE : 0);
    if (expectedSize !== data.length) fail("firmware-size", { expectedSize, size: data.length });
    if ((checksum & 0xff) !== data[paddedEnd - 1]) fail("firmware-checksum");

    if (hashAppended) {
      if (!globalThis.crypto || !globalThis.crypto.subtle) fail("browser-crypto");
      const body = data.subarray(0, data.length - ESP_SHA_TRAILER_SIZE);
      const digest = new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", body));
      if (!equalBytes(digest, data.subarray(data.length - ESP_SHA_TRAILER_SIZE))) fail("firmware-sha");
    }

    return { size: data.length, segmentCount, chip: "ESP32-C3" };
  }

  function parsePartitionTable(data) {
    const partitions = [];
    for (let offset = 0; offset + 32 <= data.length; offset += 32) {
      const entry = data.subarray(offset, offset + 32);
      if (entry.every(byte => byte === 0xff)) break;
      if (entry[0] === 0xeb && entry[1] === 0xeb) continue;
      if (entry[0] !== 0xaa || entry[1] !== 0x50) fail("partition-invalid");
      partitions.push({
        type: PARTITION_TYPES[entry[2]]?.[entry[3]] || "unknown",
        offset: leU32(entry, 4),
        size: leU32(entry, 8)
      });
    }
    return partitions;
  }

  function matchesLayout(actual, expected) {
    return actual.length === expected.length && expected.every((entry, index) =>
      actual[index].type === entry.type &&
      actual[index].offset === entry.offset &&
      actual[index].size === entry.size
    );
  }

  function identifyLayout(partitions) {
    const name = matchesLayout(partitions, CROSSVI_PARTITIONS) ? "CrossVi standard" :
      matchesLayout(partitions, LEGACY_PARTITIONS) ? "CrossVi legacy" : null;
    if (!name) fail("partition-unsupported");

    const otadata = partitions.find(partition => partition.type === "data-ota");
    const app0 = partitions.find(partition => partition.type === "app-ota_0");
    const app1 = partitions.find(partition => partition.type === "app-ota_1");
    return {
      name,
      otadataOffset: otadata.offset,
      appSlots: [app0, app1]
    };
  }

  function parseOtaEntry(data, offset) {
    const sequence = leU32(data, offset);
    const state = leU32(data, offset + 0x18);
    const storedCrc = leU32(data, offset + 0x1c);
    return { sequence, state, crcValid: storedCrc === otaSequenceCrc(sequence) };
  }

  function parseOtadata(data) {
    if (data.length < OTADATA_SIZE) fail("otadata-read");
    const entries = [parseOtaEntry(data, 0), parseOtaEntry(data, OTA_SECTOR_SIZE)];
    const eligible = entries
      .map((entry, sector) => ({ ...entry, sector }))
      .filter(entry => entry.sequence !== 0xffffffff && entry.crcValid && !INVALID_OTA_STATES.has(entry.state))
      .sort((left, right) => right.sequence - left.sequence);

    const active = eligible[0] || { sector: -1, sequence: 0 };
    const activeApp = active.sector < 0 ? 0 : (active.sequence - 1) % 2;
    const inactiveApp = 1 - activeApp;
    let newSequence = active.sequence + 1;
    while (((newSequence - 1) % 2) !== inactiveApp) newSequence++;

    return {
      entries,
      activeApp,
      inactiveApp,
      activeSequence: active.sequence,
      newSequence,
      targetSector: active.sector < 0 ? 0 : 1 - active.sector
    };
  }

  function buildOtadataSector(existing, sequence) {
    if (existing.length !== OTA_SECTOR_SIZE) fail("otadata-read");
    const sector = new Uint8Array(existing);
    setLeU32(sector, 0, sequence);
    setLeU32(sector, 0x18, OTA_STATE_NEW);
    setLeU32(sector, 0x1c, otaSequenceCrc(sequence));
    return sector;
  }

  function verifyOtaSwitch(otadata, expectedApp, expectedSequence) {
    const actual = parseOtadata(otadata);
    if (actual.activeApp !== expectedApp || actual.activeSequence !== expectedSequence) {
      fail("otadata-verify");
    }
  }

  let esptoolPromise = null;
  function loadEsptool() {
    if (!esptoolPromise) esptoolPromise = import(ESPTOOL_URL);
    return esptoolPromise;
  }

  class CrossViWebFlasher {
    constructor(port, { baudrate = 115200 } = {}) {
      this.port = port;
      this.baudrate = baudrate;
      this.loader = null;
      this.layout = null;
    }

    static requestPort() {
      if (!navigator.serial) fail("browser-serial");
      return navigator.serial.requestPort({
        filters: [{ usbVendorId: 0x303a, usbProductId: 0x1001 }]
      });
    }

    async connect() {
      const { ESPLoader, Transport } = await loadEsptool();
      const transport = new Transport(this.port, false);
      this.loader = new ESPLoader({
        transport,
        baudrate: this.baudrate,
        romBaudrate: 115200,
        enableTracing: false
      });
      await this.loader.main();
      const chip = this.loader.chip && this.loader.chip.CHIP_NAME;
      if (chip && chip !== "ESP32-C3") {
        await this.disconnect(true);
        fail("device-chip", { chip });
      }
    }

    async disconnect(skipReset = false) {
      if (!this.loader) return;
      const loader = this.loader;
      this.loader = null;
      try { await loader.transport.setDTR(false); } catch (_) {}
      try {
        if (skipReset) {
          await loader.after("no_reset_stub");
        } else {
          await loader.transport.setRTS(true);
          await new Promise(resolve => setTimeout(resolve, 100));
          await loader.after("hard_reset");
        }
      } finally {
        try {
          await loader.transport.setDTR(false);
          await loader.transport.setRTS(false);
          await new Promise(resolve => setTimeout(resolve, 100));
        } catch (_) {}
        await loader.transport.disconnect();
      }
    }

    async readLayout() {
      const data = await this.loader.readFlash(0x8000, 0x1000);
      this.layout = identifyLayout(parsePartitionTable(data));
      return this.layout;
    }

    async flashFirmware(firmware, { validated = false, onStep, onProgress, onDevice } = {}) {
      if (!validated) await validateFirmwareImage(firmware);
      let currentStep = -1;
      const step = (index, state) => {
        currentStep = index;
        if (onStep) onStep(index, state);
      };

      try {
        step(0, "running");
        await this.connect();
        step(0, "done");

        step(1, "running");
        const layout = await this.readLayout();
        step(1, "done");

        step(2, "running");
        const otaRaw = await this.loader.readFlash(layout.otadataOffset, OTADATA_SIZE);
        const ota = parseOtadata(otaRaw);
        const destination = layout.appSlots[ota.inactiveApp];
        if (firmware.length > destination.size) {
          fail("firmware-slot", { size: firmware.length, slotSize: destination.size });
        }
        if (onDevice) onDevice({ layout: layout.name, slot: `app${ota.inactiveApp}` });
        step(2, "done");

        step(3, "running");
        await this.loader.writeFlash({
          fileArray: [{ data: firmware, address: destination.offset }],
          flashSize: "keep",
          flashMode: "keep",
          flashFreq: "keep",
          eraseAll: false,
          compress: true,
          reportProgress: (_, written, total) => {
            if (onProgress) onProgress(written, total);
          }
        });
        step(3, "done");

        step(4, "running");
        const sectorOffset = ota.targetSector * OTA_SECTOR_SIZE;
        const sector = buildOtadataSector(
          otaRaw.subarray(sectorOffset, sectorOffset + OTA_SECTOR_SIZE),
          ota.newSequence
        );
        await this.loader.writeFlash({
          fileArray: [{ data: sector, address: layout.otadataOffset + sectorOffset }],
          flashSize: "keep",
          flashMode: "keep",
          flashFreq: "keep",
          eraseAll: false,
          compress: true
        });
        const verified = await this.loader.readFlash(layout.otadataOffset, OTADATA_SIZE);
        verifyOtaSwitch(verified, ota.inactiveApp, ota.newSequence);
        step(4, "done");

        step(5, "running");
        await this.disconnect(false);
        step(5, "done");
        return { layout: layout.name, slot: `app${ota.inactiveApp}` };
      } catch (error) {
        if (currentStep >= 0 && onStep) onStep(currentStep, "error");
        try { await this.disconnect(false); } catch (_) {}
        throw error;
      }
    }
  }

  return {
    FlashError,
    CrossViWebFlasher,
    CROSSVI_PARTITIONS,
    LEGACY_PARTITIONS,
    buildOtadataSector,
    identifyLayout,
    parseOtadata,
    parsePartitionTable,
    validateFirmwareImage,
    verifyOtaSwitch
  };
});
