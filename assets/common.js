// Shared by ui.html and options.html -- inlined into each page's <script>
// at build time by cmake/InlineAssets.cmake, in place of the "@common.js@"
// marker comment: the native bridge, slider painting
// and the toast.

const $ = s => document.querySelector(s);
const bridge = window.chrome && window.chrome.webview;
const post = m => { if (bridge) bridge.postMessage(m); };

// The look is fixed (see common.css): no theme override, no Windows accent.

/*@icons.js@*/
fillPix();

// Fills the slider track up to its current value (the native range has no
// styled "filled" part in Chromium).
function paintRange(el) {
  const min = +el.min, max = +el.max, v = +el.value;
  const p = ((v - min) / (max - min)) * 100;
  el.style.background =
    `linear-gradient(#e83c3c, #c01010 45%, #8a0202) 0 0 / ${p}% 100% no-repeat, var(--track-off)`;
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
