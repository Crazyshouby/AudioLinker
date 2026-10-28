"""Headless-Chrome harness for the generated main page (build-*/gen/ui.gen.html).

Stubs the WebView2 bridge, feeds the page the same kind of messages the
native side sends, drives a few clicks, and dumps the checks as JSON in the
DOM. No screenshot: errors, DOM structure and element rectangles only.
Usage: python tools/ui_smoke.py build-dev/gen/ui.gen.html   (CHROME=<exe> to override the browser)
"""
import io, json, os, re, subprocess, sys, tempfile

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
src = sys.argv[1]
html = io.open(src, encoding="utf-8").read()

STUB = r"""<script>
window.__errors = []; window.__posted = [];
// A plain browser has no native host: show every window side by side.
document.documentElement.dataset.panel = "all";
window.addEventListener("error", e => window.__errors.push("error: " + e.message));
window.addEventListener("unhandledrejection", e => window.__errors.push("rejection: " + e.reason));
window.chrome = { webview: {
  postMessage: m => window.__posted.push(m),
  addEventListener: (t, fn) => { window.__handler = fn; } } };
</script>"""

TEST = r"""<script>
setTimeout(() => {
  const dev = (id, name, checked) => ({ id, name, kind: "headphones", alias: "", color: "", checked,
    channel: 0, volume: 100, latency: 0, eq: [0,0,0,0,0], muted: false, sourcePid: 0 });
  const state = { type: "state", running: true, starting: false, version: "1.1.0", accent: "#018574",
    autostart: false, launchAtStartup: false, masterVolume: 100, groupMuted: false, captureMode: 0,
    nativeReferenceId: "", theme: 2, toggleHotkey: "Ctrl+Maj+G", switcherHotkey: "Alt+X",
    setupDigitHotkeys: true, devices: [dev("A", "Casque (8- Arctis 7 Game)", false),
      dev("B", "Casque pour téléphone (8- Arctis 7 Chat)", false), dev("C", "2 - SL27T-1 LED (3- AMD High Definition Audio Device)", true)],
    setups: [{ name: "Casque", count: 1 }, { name: "Groupe Ecrans", count: 1 }, { name: "Bluetoooth", count: 1 }],
    activeSetup: "", editingSetup: "Groupe Ecrans", playingSetup: "Groupe Ecrans", setupMic: "",
    inputs: [], automation: {} };
  const send = m => window.__handler({ data: m });
  const r = { steps: {} };
  const step = (name, fn) => { try { r.steps[name] = fn(); } catch (e) { r.steps[name] = "THROW " + e.message; } };

  step("state1", () => { send(state); return document.querySelectorAll(".setup").length; });
  step("select", () => { document.querySelector('.device[data-id="B"]').click(); return !!document.querySelector("#sheet") && !document.querySelector("#sheet").hidden; });
  step("state2_with_sheet_open", () => { send(state); return "ok"; });
  step("sheet_still_after_row", () => {
    const s = document.getElementById("sheet");
    return { inDOM: document.body.contains(s), parent: s.parentElement && s.parentElement.id, hidden: s.hidden,
             prevIsSelectedRow: !!(s.previousElementSibling && s.previousElementSibling.dataset.id === "B") };
  });
  step("setups_follow_state", () => {
    const changed = Object.assign({}, state, { playingSetup: "Casque", editingSetup: "Casque", activeSetup: "Casque" });
    send(changed);
    const playing = [...document.querySelectorAll(".setup")].filter(s => s.querySelector(".setup-play.playing")).map(s => s.querySelector(".setup-name").textContent);
    return playing;
  });
  step("click_play_posts", () => {
    window.__posted.length = 0;
    document.querySelectorAll(".setup-play")[2].click();
    return window.__posted.slice();
  });
  step("layout_setup_row_modified_playing", () => {
    send(state); // Groupe Ecrans: playing + modified (the crowded case)
    const row = [...document.querySelectorAll(".setup")].find(s => s.querySelector(".setup-name").textContent === "Groupe Ecrans");
    const R = e => { const b = e.getBoundingClientRect(); return { l: Math.round(b.left), r: Math.round(b.right), t: Math.round(b.top), b: Math.round(b.bottom) }; };
    const kids = [...row.querySelectorAll(".setup-main, .setup-save, .setup-play")].map(e => [e.className, R(e)]);
    const subs = [...row.querySelectorAll(".setup-sub > *")].map(e => [e.textContent, R(e)]);
    const overlap = (a, b) => a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b;
    const clashes = [];
    const all = kids.concat(subs.map(([t, rc]) => ["sub:" + t, rc]));
    for (let i = 0; i < all.length; i++) for (let j = i + 1; j < all.length; j++) {
      // a badge legitimately sits inside .setup-main; only flag siblings across groups
      const a = all[i], b = all[j];
      if (a[0] === "setup-main" || b[0] === "setup-main") continue;
      if (overlap(a[1], b[1])) clashes.push(a[0] + " x " + b[0]);
    }
    const name = row.querySelector(".setup-name");
    return { name: name.textContent, nameTruncated: name.scrollWidth > name.clientWidth, kids, clashes,
             rowHeight: R(row).b - R(row).t };
  });
  r.errors = window.__errors;
  const pre = document.createElement("pre"); pre.id = "__result"; pre.textContent = JSON.stringify(r);
  document.body.appendChild(pre);
}, 50);
</script>"""

page = html.replace("<head>", "<head>" + STUB, 1).replace("</body>", TEST + "</body>", 1)
tmp = os.path.join(tempfile.gettempdir(), "al_uitest.html")
io.open(tmp, "w", encoding="utf-8").write(page)
prof = tempfile.mkdtemp(prefix="al_chrome_")
cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--user-data-dir=" + prof,
       "--window-size=1018,880", "--virtual-time-budget=4000", "--dump-dom", "file:///" + tmp.replace("\\", "/")]
out = subprocess.run(cmd, capture_output=True, text=True, timeout=60, encoding="utf-8", errors="replace").stdout
m = re.search(r'<pre id="__result">(.*?)</pre>', out, re.S)
if not m:
    print("NO RESULT; DOM tail:", out[-1500:])
    sys.exit(2)
res = json.loads(m.group(1).replace("&quot;", '"').replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">"))
print(json.dumps(res, indent=2, ensure_ascii=False))
