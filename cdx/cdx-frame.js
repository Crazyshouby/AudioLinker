/* cdx-frame.js -- le cadre de fenêtre CDX et ses composants, écrits une seule fois (cdx-bridge.js doit être chargé avant ; cdx-frame.css avec cdx-ui.css).
   Le HTML attendu est celui de la charte :
     <div class="win active"><div class="tb"><button class="tb-btn tb-l" data-act="close"></button> ... <span class="tb-title">Titre</span> ...</div><div class="cb">...</div><i class="grip-rz"></i></div>
   data-act d'un .tb-btn : close | min | max (ou zoom) | shade | menu (émet l'événement « cdx:menu » sur le bouton) ; data-glyph : x | minus | plus (déduit de data-act par défaut).
   data-dblclick d'un .tb : "shade" (défaut, comme KIRC et AudioLinker) ou "max" (Explorateur, Navigateur).
   Usage :  CdxFrame.init();  CdxFrame.edges();  CdxFrame.title("Documents");  await CdxFrame.dialog({ title, text, icon, yes, no });  CdxFrame.menu(items, x, y, title) */
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
    const dbl = () => (tb.dataset.dblclick === "max" ? cdx.win.max() : cdx.win.shade());
    tb.addEventListener("pointerdown", ev => {
      if (ev.button !== 0 || ev.target.closest(".tb-btn")) return;
      const now = performance.now();
      if (now - lastDown < 400) { lastDown = -Infinity; dbl(); return; }
      lastDown = now; dragging = true;
      try { tb.setPointerCapture(ev.pointerId); } catch { /* pas de capture : le déplacement natif s'en passe */ }
      cdx.win.drag("start");
    });
    tb.addEventListener("pointermove", () => {
      if (!dragging || queued) return;
      queued = true;
      requestAnimationFrame(() => { queued = false; if (dragging) cdx.win.drag("move"); });
    });
    const end = () => { if (dragging) { dragging = false; cdx.win.drag("end"); } };
    tb.addEventListener("pointerup", end);
    tb.addEventListener("pointercancel", end);
    for (const b of tb.querySelectorAll(".tb-btn")) {
      const act = b.dataset.act; if (!act) continue;
      const g = b.dataset.glyph || DEFAULT_GLYPH[act]; if (g && !b.firstChild) b.innerHTML = GLYPH[g] || "";
      b.addEventListener("click", ev => {
        if (act === "menu") b.dispatchEvent(new CustomEvent("cdx:menu", { bubbles: true, detail: { x: ev.clientX, y: ev.clientY } }));
        else if (act === "zoom") cdx.win.max(); else cdx.win[act] && cdx.win[act]();
      });
    }
  }

  // Redimensionnement : poignée de coin (.grip-rz) et poignées de bord (.rz[data-e], codes HT de Windows : 10 g, 11 d, 12 h, 13 hg, 14 hd, 15 b, 16 bg, 17 bd).
  function resizer(el, edge) {
    let sizing = false, queued = false;
    el.addEventListener("pointerdown", ev => {
      if (ev.button !== 0) return;
      sizing = true; try { el.setPointerCapture(ev.pointerId); } catch { /* idem */ }
      cdx.win.resize("start", edge);
    });
    el.addEventListener("pointermove", () => {
      if (!sizing || queued) return;
      queued = true;
      requestAnimationFrame(() => { queued = false; if (sizing) cdx.win.resize("move", edge); });
    });
    const end = () => { if (sizing) { sizing = false; cdx.win.resize("end", edge); } };
    el.addEventListener("pointerup", end);
    el.addEventListener("pointercancel", end);
  }

  const applyTheme = name => { if (!name || name === "rouge" || name === "cdx") delete document.documentElement.dataset.theme; else document.documentElement.dataset.theme = name; };

  const CdxFrame = {
    // Branche tout le cadre de la page (barres de titre, poignées, événements de l'hôte) et annonce que la page est prête.
    init(opts = {}) {
      document.querySelectorAll(".tb").forEach(titleBar);
      document.querySelectorAll(".grip-rz").forEach(g => resizer(g, 17));
      document.querySelectorAll(".rz[data-e]").forEach(g => resizer(g, +g.dataset.e));
      cdx.on("active", v => document.body.classList.toggle("active", !!v));
      cdx.on("shade", v => document.body.classList.toggle("shaded", !!v));
      cdx.on("theme", applyTheme);
      if (opts.ready !== false) cdx.win.ready();
      return this;
    },
    // Ajoute les huit poignées de bord si la page n'en a pas (fenêtres sans cadre système redimensionnables par leur bord).
    edges() {
      if (document.querySelector(".rz")) return this;
      const codes = { l: 10, r: 11, t: 12, tl: 13, tr: 14, b: 15, bl: 16, br: 17 };
      for (const [k, e] of Object.entries(codes)) { const i = document.createElement("i"); i.className = "rz " + k; i.dataset.e = e; resizer(i, e); document.body.appendChild(i); }
      return this;
    },
    title(text) { document.querySelectorAll(".tb-title").forEach(t => { t.textContent = text; }); cdx.win.title(text); },
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

    // Menu contextuel. items : [libellé, action, { icon, key, disabled, check, sub }] ou null (séparateur). Se ferme au clic ailleurs, à Échap ou à la perte du focus de la fenêtre.
    //  - check : true/false affiche une case (cochée ou non) devant le libellé ; undefined : pas de case.
    //  - sub : un sous-menu (liste d'éléments de la même forme, ou fonction qui la rend : elle est appelée à l'ouverture, pour une liste longue ou lente à construire). Il s'ouvre au survol (120 ms)
    //    ou au clic, à droite de l'élément (à gauche s'il manque de la place), et l'`action` d'un tel élément est ignorée. Cliquer une action d'un sous-menu ferme tous les niveaux.
    menu(items, x, y, title = "") {
      document.querySelectorAll(".menu[data-cdx]").forEach(m => m.remove());
      const levels = [];   // les menus ouverts, du premier au dernier
      const closeFrom = n => { while (levels.length > n) levels.pop().el.remove(); };
      const closeAll = () => { closeFrom(0); document.removeEventListener("mousedown", away, true); document.removeEventListener("keydown", esc2, true); window.removeEventListener("blur", closeAll); };
      const away = e => { if (!levels.some(l => l.el.contains(e.target))) closeAll(); };
      const esc2 = e => { if (e.key === "Escape") closeAll(); };

      // Construit un niveau ; `anchor` (rectangle de l'élément parent) place un sous-menu à côté de lui, sinon (x, y) est la position du pointeur.
      function build(list, level, px, py, ttl, anchor) {
        closeFrom(level);
        const m = document.createElement("div"); m.className = "menu"; m.dataset.cdx = "1";
        const icons = !!window.pix && list.some(it => it && it[2] && it[2].icon);   // l'espace d'icône n'existe que si un élément en a une
        const keys = list.some(it => it && it[2] && it[2].key);
        const checks = list.some(it => it && it[2] && it[2].check !== undefined);
        m.innerHTML = (ttl ? `<div class="menu-title">${esc(ttl)}</div>` : "") + list.map((it, i) => {
          if (!it) return '<div class="menu-sep"></div>';
          const o = it[2] || {};
          return `<div class="menu-item ${o.disabled ? "off" : ""}" data-i="${i}">${checks ? `<span class="chk${o.check ? " on" : ""}"></span>` : ""}${icons ? (o.icon ? `<span class="pix">${window.pix(o.icon, 16)}</span>` : '<span style="width:16px"></span>') : ""}<span>${esc(it[0])}</span>${keys ? `<span style="margin-left:auto;padding-left:28px;color:var(--faint)">${esc(o.key || "")}</span>` : ""}${o.sub ? '<span class="subarrow"></span>' : ""}</div>`;
        }).join("");
        m.style.visibility = "hidden"; document.body.appendChild(m);
        const r = m.getBoundingClientRect();   // placement d'après la taille réelle : le menu reste dans la fenêtre
        let left = px, top = py;
        if (anchor) { left = anchor.right - 2; if (left + r.width > innerWidth - 2) left = anchor.left - r.width + 2; top = anchor.top - 6; }
        m.style.left = Math.max(2, Math.min(left, innerWidth - r.width - 2)) + "px"; m.style.top = Math.max(2, Math.min(top, innerHeight - r.height - 2)) + "px"; m.style.visibility = "";
        const lv = { el: m, list, timer: 0 }; levels.push(lv);
        const openSub = el => {
          const it = list[+el.dataset.i]; if (!it || !it[2] || !it[2].sub || (it[2] && it[2].disabled)) return;
          let sub = it[2].sub; try { if (typeof sub === "function") sub = sub(); } catch { sub = []; }
          if (!sub || !sub.length) return;
          build(sub, level + 1, 0, 0, "", el.getBoundingClientRect());
        };
        m.addEventListener("mouseover", e => {
          const el = e.target.closest("[data-i]"); if (!el || !m.contains(el)) return;
          clearTimeout(lv.timer);
          const it = list[+el.dataset.i];
          if (it && it[2] && it[2].sub) { if (levels[level + 1] && levels[level + 1].from === el) return; lv.timer = setTimeout(() => { openSub(el); if (levels[level + 1]) levels[level + 1].from = el; }, 120); }
          else lv.timer = setTimeout(() => closeFrom(level + 1), 120);   // un élément sans sous-menu : les sous-menus ouverts se referment
        });
        m.addEventListener("click", e => {
          const el = e.target.closest("[data-i]"); if (!el) return; const it = list[+el.dataset.i]; if (!it || (it[2] && it[2].disabled)) return;
          if (it[2] && it[2].sub) { clearTimeout(lv.timer); openSub(el); if (levels[level + 1]) levels[level + 1].from = el; return; }
          closeAll(); if (typeof it[1] === "function") it[1]();
        });
        return m;
      }

      build(items, 0, x, y, title, null);
      document.addEventListener("mousedown", away, true); document.addEventListener("keydown", esc2, true); window.addEventListener("blur", closeAll);
      return closeAll;
    },
  };
  window.CdxFrame = CdxFrame;
})();
