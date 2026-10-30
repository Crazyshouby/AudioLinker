/* kdx-frame.js -- le cadre de fenêtre KDX et ses composants, écrits une seule fois (kdx-bridge.js doit être chargé avant ; kdx-frame.css avec kdx-ui.css).
   Le HTML attendu est celui de la charte :
     <div class="win active"><div class="tb"><button class="tb-btn tb-l" data-act="close"></button> ... <span class="tb-title">Titre</span> ...</div><div class="cb">...</div><i class="grip-rz"></i></div>
   data-act d'un .tb-btn : close | min | max (ou zoom) | shade | menu (émet l'événement « kdx:menu » sur le bouton) ; data-glyph : x | minus | plus (déduit de data-act par défaut).
   data-dblclick d'un .tb : "shade" (défaut, comme KIRC et AudioLinker) ou "max" (Explorateur, Navigateur).
   Usage :  KdxFrame.init();  KdxFrame.edges();  KdxFrame.title("Documents");  await KdxFrame.dialog({ title, text, icon, yes, no });  KdxFrame.menu(items, x, y, title) */
(() => {
  "use strict";
  const GLYPH = {
    x: '<svg viewBox="0 0 9 9"><path d="M1 1L8 8M8 1L1 8" stroke="#fff" stroke-width="2" fill="none"/></svg>',
    minus: '<svg viewBox="0 0 9 9"><path d="M1 4.5H8" stroke="#fff" stroke-width="2" fill="none"/></svg>',
    plus: '<svg viewBox="0 0 9 9"><path d="M1 4.5H8M4.5 1V8" stroke="#fff" stroke-width="2" fill="none"/></svg>',
  };
  const DEFAULT_GLYPH = { close: "x", min: "minus", shade: "minus", max: "plus", zoom: "plus" };
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

  // Déplacement : la page annonce l'appui, le mouvement (au plus un par image) et le relâchement ; l'hôte lit le curseur. Deux appuis en moins de 400 ms :
  // enrouler (ou agrandir). Le transport « webview2 » n'a besoin que de « start » (boucle de déplacement native de Windows).
  function titleBar(tb) {
    let lastDown = -Infinity, dragging = false, queued = false;   // -Infinity : un premier appui juste après le chargement ne doit pas compter comme double (défaut de KIRC et d'AudioLinker)
    const dbl = () => (tb.dataset.dblclick === "max" ? kdx.win.max() : kdx.win.shade());
    tb.addEventListener("pointerdown", ev => {
      if (ev.button !== 0 || ev.target.closest(".tb-btn")) return;
      const now = performance.now();
      if (now - lastDown < 400) { lastDown = -Infinity; dbl(); return; }
      lastDown = now; dragging = true;
      try { tb.setPointerCapture(ev.pointerId); } catch { /* pas de capture : le déplacement natif s'en passe */ }
      kdx.win.drag("start");
    });
    tb.addEventListener("pointermove", () => {
      if (!dragging || queued) return;
      queued = true;
      requestAnimationFrame(() => { queued = false; if (dragging) kdx.win.drag("move"); });
    });
    const end = () => { if (dragging) { dragging = false; kdx.win.drag("end"); } };
    tb.addEventListener("pointerup", end);
    tb.addEventListener("pointercancel", end);
    for (const b of tb.querySelectorAll(".tb-btn")) {
      const act = b.dataset.act; if (!act) continue;
      const g = b.dataset.glyph || DEFAULT_GLYPH[act]; if (g && !b.firstChild) b.innerHTML = GLYPH[g] || "";
      b.addEventListener("click", ev => {
        if (act === "menu") b.dispatchEvent(new CustomEvent("kdx:menu", { bubbles: true, detail: { x: ev.clientX, y: ev.clientY } }));
        else if (act === "zoom") kdx.win.max(); else kdx.win[act] && kdx.win[act]();
      });
    }
  }

  // Redimensionnement : poignée de coin (.grip-rz) et poignées de bord (.rz[data-e], codes HT de Windows : 10 g, 11 d, 12 h, 13 hg, 14 hd, 15 b, 16 bg, 17 bd).
  function resizer(el, edge) {
    let sizing = false, queued = false;
    el.addEventListener("pointerdown", ev => {
      if (ev.button !== 0) return;
      sizing = true; try { el.setPointerCapture(ev.pointerId); } catch { /* idem */ }
      kdx.win.resize("start", edge);
    });
    el.addEventListener("pointermove", () => {
      if (!sizing || queued) return;
      queued = true;
      requestAnimationFrame(() => { queued = false; if (sizing) kdx.win.resize("move", edge); });
    });
    const end = () => { if (sizing) { sizing = false; kdx.win.resize("end", edge); } };
    el.addEventListener("pointerup", end);
    el.addEventListener("pointercancel", end);
  }

  const applyTheme = name => { if (!name || name === "rouge" || name === "kdx") delete document.documentElement.dataset.theme; else document.documentElement.dataset.theme = name; };

  const KdxFrame = {
    // Branche tout le cadre de la page (barres de titre, poignées, événements de l'hôte) et annonce que la page est prête.
    init(opts = {}) {
      document.querySelectorAll(".tb").forEach(titleBar);
      document.querySelectorAll(".grip-rz").forEach(g => resizer(g, 17));
      document.querySelectorAll(".rz[data-e]").forEach(g => resizer(g, +g.dataset.e));
      kdx.on("active", v => document.body.classList.toggle("active", !!v));
      kdx.on("shade", v => document.body.classList.toggle("shaded", !!v));
      kdx.on("theme", applyTheme);
      if (opts.ready !== false) kdx.win.ready();
      return this;
    },
    // Ajoute les huit poignées de bord si la page n'en a pas (fenêtres sans cadre système redimensionnables par leur bord).
    edges() {
      if (document.querySelector(".rz")) return this;
      const codes = { l: 10, r: 11, t: 12, tl: 13, tr: 14, b: 15, bl: 16, br: 17 };
      for (const [k, e] of Object.entries(codes)) { const i = document.createElement("i"); i.className = "rz " + k; i.dataset.e = e; resizer(i, e); document.body.appendChild(i); }
      return this;
    },
    title(text) { document.querySelectorAll(".tb-title").forEach(t => { t.textContent = text; }); kdx.win.title(text); },
    applyTheme,

    // Boîte de dialogue modale. Résout true (bouton par défaut, un seul par dialogue) ou false (Non, Échap).
    dialog({ title = "", text = "", icon = "", yes = "Oui", no = "Non" } = {}) {
      return new Promise(resolve => {
        const v = document.createElement("div"); v.className = "veil";
        const ic = icon && window.pix ? `<span class="pix">${window.pix(icon, 32)}</span>` : "";
        v.innerHTML = `<div class="win active"><div class="tb"><span class="tb-title">${esc(title)}</span></div><div class="cb"><div class="dlg-body">${ic}<div class="dlg-text">${esc(text)}</div></div><div class="btns"><button class="primary" data-y>${esc(yes)}</button>${no ? `<button class="btn" data-n>${esc(no)}</button>` : ""}</div></div></div>`;
        const done = r => { v.remove(); resolve(r); };
        v.querySelector("[data-y]").onclick = () => done(true);
        const n = v.querySelector("[data-n]"); if (n) n.onclick = () => done(false);
        v.addEventListener("keydown", e => { if (e.key === "Escape") done(false); });
        document.body.appendChild(v); v.querySelector("[data-y]").focus();
      });
    },

    // Menu contextuel. items : [libellé, action, { icon, key, disabled }] ou null (séparateur). Se ferme au clic ailleurs ou à Échap.
    menu(items, x, y, title = "") {
      document.querySelectorAll(".menu[data-kdx]").forEach(m => m.remove());
      const m = document.createElement("div"); m.className = "menu"; m.dataset.kdx = "1";
      m.innerHTML = (title ? `<div class="menu-title">${esc(title)}</div>` : "") + items.map((it, i) => !it ? '<div class="menu-sep"></div>' :
        `<div class="menu-item ${it[2] && it[2].disabled ? "off" : ""}" data-i="${i}">${it[2] && it[2].icon && window.pix ? `<span class="pix">${window.pix(it[2].icon, 16)}</span>` : '<span style="width:16px"></span>'}<span>${esc(it[0])}</span><span style="margin-left:auto;padding-left:28px;color:var(--faint)">${esc((it[2] && it[2].key) || "")}</span></div>`).join("");
      m.style.left = Math.max(0, Math.min(x, innerWidth - 260)) + "px"; m.style.top = Math.max(0, Math.min(y, innerHeight - 40 * items.length)) + "px";
      const close = () => { m.remove(); document.removeEventListener("mousedown", away, true); document.removeEventListener("keydown", esc2, true); };
      const away = e => { if (!m.contains(e.target)) close(); };
      const esc2 = e => { if (e.key === "Escape") close(); };
      m.addEventListener("click", e => { const el = e.target.closest("[data-i]"); if (!el) return; const it = items[+el.dataset.i]; if (it[2] && it[2].disabled) return; close(); it[1](); });
      document.body.appendChild(m);
      document.addEventListener("mousedown", away, true); document.addEventListener("keydown", esc2, true);
      return close;
    },
  };
  window.KdxFrame = KdxFrame;
})();
