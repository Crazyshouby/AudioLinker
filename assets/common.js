// Shared by ui.html and options.html -- inlined into each page's <script>
// at build time by cmake/InlineAssets.cmake, in place of the "@common.js@"
// marker comment: the native bridge, slider painting
// and the toast.

const $ = s => document.querySelector(s);
const bridge = window.chrome && window.chrome.webview;
const post = m => { if (bridge) bridge.postMessage(m); };

// The look is the CDX-UI charter (see common.css): no theme override, no Windows accent.

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
// The frame is cdx-frame.js, shared by the whole CDX suite, talking to the native side through cdx-bridge.js (its "strings" transport, selected
// by data-cdx-host="strings" on <html>: dragstart / dragmove / dragend, resizewin, shade, closepanel). The native side owns the window: the page
// only asks. data-act="close" closes this window (the launcher: hides everything to the tray), "shade" rolls it up to its title bar.
/*@cdx-bridge.js@*/
/*@cdx-frame.js@*/
CdxFrame.init();

// The accent of the CDX suite (%APPDATA%\CDX\suite.json): the page takes the theme of themes.css of that name.
const applySuiteAccent = a => { if (a) CdxFrame.applyTheme(a); };

// Messages every window understands. "active" (Windows has activated the window: the red frame), "shade" (rolled up) and "theme" (the suite's
// accent changed) are handled by cdx-frame.js, which listens to the same messages: true = nothing more for the page to do with them.
function chromeMessage(m) {
  if (m.type === "active" || m.type === "shade" || m.type === "theme") return true;
  if (m.type === "state" && m.suiteAccent) applySuiteAccent(m.suiteAccent); // and the page still handles the state itself
  return false;
}
