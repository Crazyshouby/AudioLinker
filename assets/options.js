// Script of options.html (the shared one is common.js). Inlined into the page at build time, after common.js, inside its "use strict" script.

let state = {
  autostart: false, launchAtStartup: false,
  cushionMs: 150, cushionMsMin: 30, cushionMsMax: 800,
  deviceFillMs: 60, deviceFillMsMin: 20, deviceFillMsMax: 150,
  lowLatency: false, optimizing: false, running: false,
  toggleHotkey: "Ctrl+Alt+G", switcherHotkey: "Ctrl+Alt+S", capturingHotkey: "",
  setupDigitHotkeys: true,
};

// Cushion + device fill dominate the group's latency vs. native playback;
// ~20ms covers the capture side (virtual cable + polling). Purely
// indicative -- Bluetooth links add their own (per-device) latency on top.
function renderLatencyEstimate() {
  const total = (state.cushionMs ?? 150) + (state.deviceFillMs ?? 60) + 20;
  $("#latencyEstimate").textContent =
    `Latence totale du groupe ≈ ${total} ms (hors latence propre au Bluetooth, mesurée par enceinte dans la liste).`;
}

function renderAll() {
  $("#versionLabel").textContent = state.version ? "AudioLinker " + state.version : "";
  $("#autostart").checked = state.autostart;
  $("#launchWin").checked = !!state.launchAtStartup;

  const el = $("#sCushion");
  el.min = state.cushionMsMin ?? 30;
  el.max = state.cushionMsMax ?? 800;
  el.value = state.cushionMs ?? 150;
  paintRange(el);
  $("#vCushion").textContent = (state.cushionMs ?? 150) + " ms";

  const fill = $("#sDeviceFill");
  fill.min = state.deviceFillMsMin ?? 20;
  fill.max = state.deviceFillMsMax ?? 150;
  fill.value = state.deviceFillMs ?? 60;
  paintRange(fill);
  $("#vDeviceFill").textContent = (state.deviceFillMs ?? 60) + " ms";
  $("#lowLatency").checked = !!state.lowLatency;
  const optBtn = $("#optimizeBtn");
  optBtn.textContent = state.optimizing ? "Optimisation en cours… (annuler)"
                                        : "Optimiser la latence automatiquement";
  optBtn.classList.toggle("on", !!state.optimizing);
  optBtn.disabled = !state.running && !state.optimizing;
  optBtn.title = state.running || state.optimizing
    ? "Réduit automatiquement la marge jusqu'au réglage le plus bas sans craquement."
    : "Démarrez le groupe d'abord, puis lancez l'optimisation.";
  renderLatencyEstimate();

  const capturing = state.capturingHotkey || "";
  const listening = "Ctrl/Alt/Maj/Win + une touche… (Échap pour annuler)";
  $("#hkToggleBtn").textContent = capturing === "toggle" ? listening : (state.toggleHotkey || "Ctrl+Alt+G");
  $("#hkSwitcherBtn").textContent = capturing === "switcher" ? listening : (state.switcherHotkey || "Ctrl+Alt+S");
  $("#setupDigits").checked = state.setupDigitHotkeys !== false;

  $("#captureMode").value = String(state.captureMode ?? 0);
  $("#captureHelp").textContent = (state.captureMode ?? 0) === 1
    ? "Sans câble : le son est capturé directement (toutes les applications sauf AudioLinker) et la sortie " +
      "par défaut de Windows continue de jouer en natif, hors du groupe, comme référence. Le groupe arrive " +
      "forcément après elle (marge + tampon) : lancez le calage micro pour mesurer l'écart et savoir si votre " +
      "configuration permet la synchronisation. Pas de sources par application dans ce mode."
    : "Câble virtuel : le groupe devient la sortie par défaut de Windows ; toutes les enceintes, y compris " +
      "celle qui était par défaut, sont synchronisées entre elles.";
}

$("#captureMode").addEventListener("change", () => {
  post("capturemode|" + $("#captureMode").value);
});

$("#setupDigits").addEventListener("change", () => {
  post("setupdigits|" + ($("#setupDigits").checked ? 1 : 0));
});

$("#autostart").addEventListener("change", () => {
  state.autostart = $("#autostart").checked;
  post("autostart|" + (state.autostart ? 1 : 0));
});
$("#launchWin").addEventListener("change", () => {
  state.launchAtStartup = $("#launchWin").checked;
  post("launchwindows|" + (state.launchAtStartup ? 1 : 0));
});

$("#sCushion").addEventListener("input", () => {
  const el = $("#sCushion");
  state.cushionMs = +el.value;
  paintRange(el);
  $("#vCushion").textContent = state.cushionMs + " ms";
  renderLatencyEstimate();
  post("cushion|" + state.cushionMs);
});
$("#sCushion").addEventListener("change", () => post("cushioncommit"));

$("#sDeviceFill").addEventListener("input", () => {
  const el = $("#sDeviceFill");
  state.deviceFillMs = +el.value;
  paintRange(el);
  $("#vDeviceFill").textContent = state.deviceFillMs + " ms";
  renderLatencyEstimate();
  post("devicefill|" + state.deviceFillMs);
});
$("#sDeviceFill").addEventListener("change", () => post("devicefillcommit"));

$("#lowLatency").addEventListener("change", () => {
  state.lowLatency = $("#lowLatency").checked;
  post("lowlatency|" + (state.lowLatency ? 1 : 0));
});

$("#optimizeBtn").addEventListener("click", () => {
  post("optimize|" + (state.optimizing ? "cancel" : "start"));
});

$("#openLogBtn").addEventListener("click", () => post("openlog"));

// The actual key combo is captured natively (a low-level keyboard hook on
// the C++ side, see StartHotkeyCapture/HotkeyCaptureHookProc) instead of a
// page-level keydown listener: WebView2/Chromium reserves many Ctrl(+Shift)
// +<letter> and F-key combos as its own browser accelerators and never
// delivers those to the page's JS. This side just tells the native side
// when to start/stop listening and reflects state.capturingHotkey.
$("#hkToggleBtn").addEventListener("click", () => post("capturehotkey|toggle"));
$("#hkSwitcherBtn").addEventListener("click", () => post("capturehotkey|switcher"));

$("#exportBtn").addEventListener("click", () => post("exportconfig"));
$("#importBtn").addEventListener("click", () => post("importconfig"));
$("#resetBtn").addEventListener("click", () => post("resetoptions"));

window.addEventListener("keydown", ev => {
  if (ev.key === "Escape") post("cancelhotkeycapture");
});

if (bridge) {
  bridge.addEventListener("message", ev => {
    const m = ev.data;
    if (!m || !m.type) return;
    if (chromeMessage(m)) return;
    if (m.type === "state") {
      state = m;
      renderAll();
    } else if (m.type === "error") {
      toast(m.message);
    } else if (m.type === "info") {
      toast(m.message, "ok");
    }
  });
  post("ready");
}
