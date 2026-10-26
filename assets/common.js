// Shared by ui.html and options.html -- inlined into each page's <script>
// at build time by cmake/InlineAssets.cmake, in place of the "@common.js@"
// marker comment: the native bridge, theme/accent plumbing, slider painting
// and the toast.

const $ = s => document.querySelector(s);
const bridge = window.chrome && window.chrome.webview;
const post = m => { if (bridge) bridge.postMessage(m); };

// Options window's theme override: 0 = auto (let prefers-color-scheme
// decide), 1 = forced light, 2 = forced dark -- see the :root[data-theme]
// rules in common.css.
function applyTheme(theme) {
  document.documentElement.dataset.theme = theme === 1 ? "light" : theme === 2 ? "dark" : "";
}

// Recolors the UI from the Windows accent color (sent in each state
// message). --accent-2 is a lightened variant for gradients, --accent-rgb
// feeds every translucent tint; --accent-glow derives from it in CSS.
function applyAccent(hex) {
  if (!hex || !/^#[0-9a-f]{6}$/i.test(hex)) return;
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  const lift = c => Math.round(c + (255 - c) * 0.35);
  const hex2 = "#" + [lift(r), lift(g), lift(b)].map(v => v.toString(16).padStart(2, "0")).join("");
  const rootStyle = document.documentElement.style;
  rootStyle.setProperty("--accent", hex);
  rootStyle.setProperty("--accent-2", hex2);
  rootStyle.setProperty("--accent-rgb", `${r}, ${g}, ${b}`);
}

// Fills the slider track up to its current value (the native range has no
// styled "filled" part in Chromium).
function paintRange(el) {
  const min = +el.min, max = +el.max, v = +el.value;
  const p = ((v - min) / (max - min)) * 100;
  el.style.background =
    `linear-gradient(to right, var(--accent) 0%, var(--accent) ${p}%, var(--track-off) ${p}%, var(--track-off) 100%)`;
}

// Transient message at the bottom of the window. kind "ok" = green info
// instead of red error. action: optional {cmd, label} -- a button inside
// the toast that posts `cmd` back to the native side (e.g. "Reprendre" when
// the default output was changed behind the group's back).
let toastTimer = 0;
function toast(msg, kind, action) {
  const t = $("#toast");
  t.textContent = msg;
  if (action) {
    const btn = document.createElement("button");
    btn.className = "ghost-btn toast-action";
    btn.textContent = action.label;
    btn.addEventListener("click", () => { post(action.cmd); t.classList.remove("show"); });
    t.appendChild(btn);
  }
  t.classList.toggle("ok", kind === "ok");
  t.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove("show"), action ? 15000 : 6000);
}
