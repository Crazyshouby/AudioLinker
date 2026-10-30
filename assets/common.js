// Shared by ui.html and options.html -- inlined into each page's <script>
// at build time by cmake/InlineAssets.cmake, in place of the "@common.js@"
// marker comment: the native bridge, slider painting
// and the toast.

const $ = s => document.querySelector(s);
const bridge = window.chrome && window.chrome.webview;
const post = m => { if (bridge) bridge.postMessage(m); };

// The look is the KDX-UI charter (see common.css): no theme override, no Windows accent.

// Which window this page is (set by the native side before the page runs):
// launcher, outputs, setups, levels, journal, options -- or "all" in a plain
// browser (tests), where every window is shown.
// The native side leaves the name in window.__panel (a script that runs
// before the document exists); it is copied onto <html> for the CSS.
const PANEL = window.__panel || document.documentElement.dataset.panel || "";
if (PANEL) document.documentElement.dataset.panel = PANEL;

/*@icons.js@*/
fillPix();

// Fills the slider track up to its current value (the native range has no
// styled "filled" part in Chromium).
function paintRange(el) {
  const min = +el.min, max = +el.max, v = +el.value;
  const p = ((v - min) / (max - min)) * 100;
  el.style.background =
    `var(--bar) 0 0 / ${p}% 100% no-repeat, var(--frame)`;
}

// Transient messages stacked at the bottom of the window (#toasts): up to
// three at once, each gone after a few seconds or on click, so two messages
// arriving together ("source closed", then an error) no longer overwrite
// each other. kind "ok" = green info instead of red error. action:
// optional {cmd, label} -- a button inside the toast that posts `cmd` back
// to the native side (e.g. "Reprendre" when the default output was changed
// behind the group's back); such a toast stays longer.
const TOAST_MAX = 3;
function toast(msg, kind, action) {
  const box = $("#toasts");
  if (!box) return;
  const t = document.createElement("div");
  t.className = "toast" + (kind === "ok" ? " ok" : "");
  const text = document.createElement("span");
  text.className = "toast-text";
  text.textContent = msg;
  t.appendChild(text);
  if (action) {
    const btn = document.createElement("button");
    btn.className = "ghost-btn toast-action";
    btn.textContent = action.label;
    btn.addEventListener("click", ev => { ev.stopPropagation(); post(action.cmd); dismiss(); });
    t.appendChild(btn);
  }
  let timer = 0;
  const dismiss = () => {
    clearTimeout(timer);
    t.classList.remove("show");
    setTimeout(() => t.remove(), 250);
  };
  t.addEventListener("click", dismiss);
  while (box.children.length >= TOAST_MAX) box.firstChild.remove();
  box.appendChild(t);
  requestAnimationFrame(() => t.classList.add("show"));
  timer = setTimeout(dismiss, action ? 15000 : 5000);
}

// ---------- Window chrome (title bar buttons, drag, resize grip) ----------
// The native side owns the window: the page only asks. data-act="close"
// closes this window (the launcher: hides everything to the tray),
// data-act="shade" rolls it up to its title bar; data-glyph picks the icon.
const TB_GLYPHS = {
  x: '<svg viewBox="0 0 9 9"><path d="M1 1L8 8M8 1L1 8" stroke="#fff" stroke-width="2" fill="none"/></svg>',
  minus: '<svg viewBox="0 0 9 9"><path d="M1 4.5H8" stroke="#fff" stroke-width="2" fill="none"/></svg>',
};
function initChrome() {
  for (const tb of document.querySelectorAll(".tb")) {
    // Moving: the page reports press / movement / release, the native side
    // reads the cursor and moves the window (a system move loop would take
    // the mouse over and the page would never see the second press of a
    // double click). Two presses within 400 ms roll the window up or down.
    let lastDown = 0, dragging = false, moveQueued = false;
    tb.addEventListener("pointerdown", ev => {
      if (ev.button !== 0 || ev.target.closest(".tb-btn")) return;
      const now = performance.now();
      if (now - lastDown < 400) { lastDown = 0; post("shade"); return; }
      lastDown = now;
      dragging = true;
      tb.setPointerCapture(ev.pointerId);
      post("dragstart");
    });
    tb.addEventListener("pointermove", () => {
      if (!dragging || moveQueued) return;
      moveQueued = true;
      requestAnimationFrame(() => { moveQueued = false; if (dragging) post("dragmove"); });
    });
    const endDrag = () => { if (dragging) { dragging = false; post("dragend"); } };
    tb.addEventListener("pointerup", endDrag);
    tb.addEventListener("pointercancel", endDrag);
    for (const b of tb.querySelectorAll(".tb-btn")) {
      b.innerHTML = TB_GLYPHS[b.dataset.glyph] || TB_GLYPHS.minus;
      b.addEventListener("click", () => post(b.dataset.act === "close" ? "closepanel" : "shade"));
    }
  }
  for (const g of document.querySelectorAll(".grip-rz")) {
    g.addEventListener("mousedown", ev => { if (ev.button === 0) post("resizewin"); });
  }
}
initChrome();

// Messages every window understands: whether Windows has activated it (the
// red frame) and whether it is rolled up. True = handled.
// The accent of the KDX suite (%APPDATA%\KDX\suite.json): the page takes the theme of themes.css of that name.
const applySuiteAccent = a => { if (a) document.documentElement.dataset.theme = a; };

function chromeMessage(m) {
  if (m.type === "theme") { applySuiteAccent(m.value); return true; }
  if (m.type === "state" && m.suiteAccent) applySuiteAccent(m.suiteAccent); // and the page still handles the state itself
  if (m.type === "active") { document.body.classList.toggle("active", !!m.value); return true; }
  if (m.type === "shade") { document.body.classList.toggle("shaded", !!m.value); return true; }
  return false;
}
