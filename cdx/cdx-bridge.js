/* cdx-bridge.js -- le pont entre une page CDX et son application hôte.
   Un seul contrat de fenêtre (docs/CONTRAT-FENETRE.md), trois transports :
     tauri     KIRC : window.__TAURI__ (commandes win_ready / win_close / win_shade / win_zoom / win_drag, événements active / shade / theme)
     strings   AudioLinker : chaînes « cmd|arg|arg » par chrome.webview.postMessage (dragstart, dragmove, dragend, resizewin, shade, closepanel)
     webview2  les applications C# (shell, Explorateur, Navigateur) : objets JSON { cmd: "win.drag", ... } et messages { type, value }
   Le transport est détecté ; <html data-cdx-host="strings"> force AudioLinker (impossible à distinguer d'un hôte JSON par détection).
   Usage :  cdx.win.drag("start") · cdx.win.resize("start", 17) · cdx.win.close() · cdx.on("theme", name => ...) · cdx.send("ma-commande", { a: 1 }) */
(() => {
  "use strict";
  const html = document.documentElement;
  const ts = window.__TAURI__, wv = window.chrome && window.chrome.webview;
  const host = html.dataset.cdxHost || (ts && ts.core ? "tauri" : wv ? "webview2" : "none");
  const listeners = {};
  const emit = (type, value, raw) => (listeners[type] || []).slice().forEach(f => { try { f(value, raw); } catch (e) { console.error("cdx.on(" + type + ")", e); } });

  let send, listen = () => { };           // listen(type) : s'abonne côté hôte quand le transport l'exige
  const win = {};

  if (host === "tauri") {
    const invoke = ts.core.invoke;
    send = (cmd, args) => invoke(cmd, args);
    // Écoute limitée à cette fenêtre : un écouteur de toute l'application recevrait aussi ce qui est envoyé aux autres fenêtres (KIRC en ouvre plusieurs).
    const here = ts.webviewWindow && ts.webviewWindow.getCurrentWebviewWindow ? ts.webviewWindow.getCurrentWebviewWindow() : ts.event;
    listen = type => here.listen(type, e => emit(type, e.payload, e));
    Object.assign(win, {
      ready: () => send("win_ready"), close: () => send("win_close"), shade: () => send("win_shade"), max: () => send("win_zoom"),
      min: () => { }, title: () => { },
      drag: phase => send("win_drag", { phase }),
      resize: phase => send("win_drag", { phase: phase === "start" ? "rstart" : phase === "move" ? "rmove" : "end" }),
    });
  } else if (host === "strings") {
    const post = s => wv && wv.postMessage(s);
    send = (cmd, args) => post([cmd].concat(args === undefined ? [] : Array.isArray(args) ? args : [args]).join("|"));
    Object.assign(win, {
      ready: () => { }, close: () => post("closepanel"), shade: () => post("shade"), max: () => { }, min: () => { }, title: () => { },
      drag: phase => post("drag" + phase),
      resize: phase => { if (phase === "start") post("resizewin"); },   // le déplacement du bord est repris par Windows (WM_NCLBUTTONDOWN)
    });
  } else if (host === "webview2") {
    const post = o => wv.postMessage(o);
    send = (cmd, args) => post(Object.assign({ cmd }, args));
    Object.assign(win, {
      ready: () => post({ cmd: "win.ready" }), close: () => post({ cmd: "win.close" }), shade: () => post({ cmd: "win.shade" }),
      max: () => post({ cmd: "win.max" }), min: () => post({ cmd: "win.min" }), title: t => post({ cmd: "win.title", t }),
      drag: phase => { if (phase === "start") post({ cmd: "win.drag" }); },            // boucle de déplacement native : « move » et « end » sont inutiles
      resize: (phase, edge) => { if (phase === "start") post({ cmd: "win.resize", edge: edge || 17 }); },
    });
  } else {
    send = (cmd, args) => console.debug("[cdx] (sans hôte)", cmd, args);
    for (const k of ["ready", "close", "shade", "max", "min", "title", "drag", "resize"]) win[k] = (...a) => console.debug("[cdx] (sans hôte) win." + k, ...a);
  }

  if (host === "webview2" || host === "strings") {
    wv.addEventListener("message", ev => {
      let m = ev.data;
      if (typeof m === "string") { try { m = JSON.parse(m); } catch { return; } }
      if (m && m.type) emit(m.type, m.value !== undefined ? m.value : m, m);
    });
  }

  window.cdx = {
    host, win, send,
    version: "1.0.0",
    on(type, fn) { if (!listeners[type]) { listeners[type] = []; listen(type); } listeners[type].push(fn); return () => this.off(type, fn); },
    off(type, fn) { listeners[type] = (listeners[type] || []).filter(f => f !== fn); },
    emit,   // pour les tests : simule un événement venu de l'hôte
  };
})();
