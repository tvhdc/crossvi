(function () {
  "use strict";

  window.addEventListener("DOMContentLoaded", () => {
    const core = window.CrossViFlasher;
    const tools = window.CrossViTools;
    if (!core || !tools) return;

    const byId = id => document.getElementById(id);
    const ui = {
      file: byId("firmwareFile"),
      fileStatus: byId("firmwareFileStatus"),
      start: byId("flashStart"),
      status: byId("flashStatus"),
      badge: byId("flashBadge"),
      progress: byId("flashProgress"),
      progressText: byId("flashProgressText"),
      device: byId("flashDevice"),
      target: byId("flashTarget"),
      browserWarning: byId("flashBrowserWarning"),
      steps: [...document.querySelectorAll("[data-flash-step]")]
    };

    let firmware = null;
    let fileInfo = null;
    let busy = false;
    let statusState = null;

    function resetSteps() {
      ui.steps.forEach(step => { step.dataset.state = "idle"; });
      ui.progress.value = 0;
      ui.progressText.textContent = "0%";
      ui.device.textContent = tools.t("flashNotDetected");
      ui.target.textContent = tools.t("flashNotSelected");
    }

    function setStatus(kind, key, values = {}) {
      statusState = key ? { kind, key, values } : null;
      ui.status.className = `statusBox${kind ? ` ${kind}` : ""}${key ? "" : " hidden"}`;
      ui.status.textContent = key ? tools.t(key, values) : "";
    }

    function setBadge(kind, key) {
      ui.badge.className = `badge${kind ? ` ${kind}` : ""}`;
      ui.badge.dataset.i18n = key;
      ui.badge.textContent = tools.t(key);
    }

    function refreshLanguage() {
      if (fileInfo) {
        ui.fileStatus.textContent = tools.t("flashFileReady", fileInfo);
      } else if (!firmware) {
        ui.fileStatus.textContent = tools.t("flashFileHint");
      }
      if (statusState) ui.status.textContent = tools.t(statusState.key, statusState.values);
      if (!busy && firmware) ui.start.textContent = tools.t("flashStart");
    }

    function errorText(error) {
      if (error && error.name === "NotFoundError") return { key: "flashCancelled", values: {} };
      const keyByCode = {
        "browser-serial": "flashBrowserUnsupported",
        "browser-crypto": "flashBrowserUnsupported",
        "firmware-read": "flashFirmwareReadError",
        "firmware-small": "flashFirmwareInvalid",
        "firmware-large": "flashFirmwareTooLarge",
        "firmware-magic": "flashFirmwareInvalid",
        "firmware-chip": "flashFirmwareWrongChip",
        "firmware-segments": "flashFirmwareInvalid",
        "firmware-truncated": "flashFirmwareInvalid",
        "firmware-size": "flashFirmwareInvalid",
        "firmware-checksum": "flashFirmwareCorrupt",
        "firmware-sha": "flashFirmwareCorrupt",
        "firmware-slot": "flashFirmwareTooLarge",
        "device-chip": "flashDeviceWrongChip",
        "partition-invalid": "flashPartitionUnsupported",
        "partition-unsupported": "flashPartitionUnsupported",
        "otadata-read": "flashOtaError",
        "otadata-verify": "flashOtaError"
      };
      const key = keyByCode[error && error.code];
      if (key) return { key, values: error.details || {} };
      return {
        key: "flashUnknownError",
        values: { message: error && error.message ? error.message : String(error) }
      };
    }

    function updateStartState() {
      const supported = Boolean(navigator.serial && globalThis.crypto && globalThis.crypto.subtle);
      ui.start.disabled = busy || !firmware || !supported;
      ui.browserWarning.classList.toggle("hidden", supported);
    }

    ui.file.addEventListener("change", async () => {
      firmware = null;
      fileInfo = null;
      resetSteps();
      setBadge("", "flashDisconnected");
      setStatus("", null);
      const file = ui.file.files && ui.file.files[0];
      if (!file) {
        ui.fileStatus.textContent = tools.t("flashFileHint");
        updateStartState();
        return;
      }

      ui.fileStatus.textContent = tools.t("flashCheckingFile");
      try {
        const candidate = new Uint8Array(await file.arrayBuffer());
        await core.validateFirmwareImage(candidate);
        firmware = candidate;
        fileInfo = { name: file.name, size: tools.formatSize(file.size) };
        ui.fileStatus.textContent = tools.t("flashFileReady", fileInfo);
        setBadge("solid", "flashReady");
      } catch (error) {
        const failure = errorText(error);
        setStatus("error", failure.key, failure.values);
        setBadge("", "flashInvalidFile");
        ui.fileStatus.textContent = tools.t("flashFileHint");
      }
      updateStartState();
    });

    ui.start.addEventListener("click", async () => {
      if (!firmware || busy) return;
      busy = true;
      updateStartState();
      setStatus("", null);
      resetSteps();
      setBadge("transparent", "flashConnecting");
      ui.start.textContent = tools.t("flashWorking");

      try {
        // Keep this request directly in the click handler: WebSerial requires
        // a user gesture before the first await.
        const port = await core.CrossViWebFlasher.requestPort();
        const flasher = new core.CrossViWebFlasher(port);
        const result = await flasher.flashFirmware(firmware, {
          validated: true,
          onStep: (index, state) => {
            if (ui.steps[index]) ui.steps[index].dataset.state = state;
          },
          onProgress: (written, total) => {
            const percent = total ? Math.min(100, Math.round((written * 100) / total)) : 0;
            ui.progress.value = percent;
            ui.progressText.textContent = `${percent}%`;
          },
          onDevice: ({ model, slot }) => {
            ui.device.textContent = tools.t("flashDetectedDevice", { model });
            ui.target.textContent = slot;
          }
        });
        ui.progress.value = 100;
        ui.progressText.textContent = "100%";
        setBadge("solid", "flashCompleteBadge");
        setStatus("", "flashComplete", { model: result.model });
      } catch (error) {
        const failure = errorText(error);
        setBadge("", "flashFailedBadge");
        setStatus("error", failure.key, failure.values);
      } finally {
        busy = false;
        ui.start.textContent = tools.t("flashStart");
        updateStartState();
      }
    });

    window.addEventListener("crossvi-language-change", refreshLanguage);
    window.addEventListener("beforeunload", event => {
      if (!busy) return;
      event.preventDefault();
      event.returnValue = "";
    });

    resetSteps();
    updateStartState();
    refreshLanguage();
  });
})();
